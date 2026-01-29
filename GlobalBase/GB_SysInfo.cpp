#include "GB_SysInfo.h"
#include "GB_Config.h"
#include "GB_Utf8String.h"
#include <algorithm>
#include <set>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <cctype>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <powerbase.h>
#include <powrprof.h>
#pragma comment(lib, "PowrProf.lib")


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
    //
    // 注意：
    // - CoInitializeSecurity 是进程级安全初始化，通常只能成功设置一次；若重复调用，常见返回 RPC_E_TOO_LATE。
    // - CoInitializeEx 可能返回 RPC_E_CHANGED_MODE（线程已用其他并发模型初始化过），此时可继续使用 COM，但不要 CoUninitialize。
    class ComScope
    {
    public:
        ComScope()
        {
            m_hrInitCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (m_hrInitCom == RPC_E_CHANGED_MODE)
            {
                m_needUninit = false;
                m_hrInitCom = S_OK;
            }
            else if (SUCCEEDED(m_hrInitCom))
            {
                m_needUninit = true;
            }
            else
            {
                m_needUninit = false;
                return;
            }

            m_hrInitSec = ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);

            if (m_hrInitSec == RPC_E_TOO_LATE)
            {
                m_hrInitSec = S_OK;
            }
        }

        ~ComScope()
        {
            if (m_needUninit)
            {
                ::CoUninitialize();
            }
        }

        bool IsOk() const
        {
            return SUCCEEDED(m_hrInitCom) && SUCCEEDED(m_hrInitSec);
        }

    private:
        bool m_needUninit = false;
        HRESULT m_hrInitCom = E_FAIL;
        HRESULT m_hrInitSec = E_FAIL;
    };

    // BSTR RAII：避免将 wchar_t* 字面量强转为 BSTR
    class BstrScope
    {
    public:
        BstrScope() = default;

        explicit BstrScope(const wchar_t* s)
        {
            if (s)
            {
                m_bstr = ::SysAllocString(s);
            }
        }

        explicit BstrScope(const std::wstring& s)
        {
            m_bstr = ::SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
        }

        BstrScope(const BstrScope&) = delete;
        BstrScope& operator=(const BstrScope&) = delete;

        BstrScope(BstrScope&& other) noexcept
        {
            m_bstr = other.m_bstr;
            other.m_bstr = nullptr;
        }

        BstrScope& operator=(BstrScope&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                m_bstr = other.m_bstr;
                other.m_bstr = nullptr;
            }
            return *this;
        }

        ~BstrScope()
        {
            Reset();
        }

        BSTR Get() const
        {
            return m_bstr;
        }

        bool IsValid() const
        {
            return m_bstr != nullptr;
        }

    private:
        void Reset()
        {
            if (m_bstr)
            {
                ::SysFreeString(m_bstr);
                m_bstr = nullptr;
            }
        }

        BSTR m_bstr = nullptr;
    };

    static string BstrToUtf8(BSTR b);

    // WMI 读取 Win32_Processor.ProcessorId（首个处理器）
    static string GetProcessorIdViaWmi()
    {
        ComScope com;
        if (!com.IsOk())
        {
            return "";
        }

        IWbemLocator* pLoc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            return "";
        }

        IWbemServices* pSvc = nullptr;
        const BstrScope rootPath(L"ROOT\\CIMV2");
        if (!rootPath.IsValid())
        {
            pLoc->Release();
            return "";
        }

        HRESULT hr = pLoc->ConnectServer(rootPath.Get(), nullptr, nullptr, 0, 0, 0, 0, &pSvc);

        pLoc->Release();
        if (FAILED(hr))
        {
            return "";
        }

        // 设置代理安全
        (void)CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

        IEnumWbemClassObject* pEnum = nullptr;
        const BstrScope wqlLang(L"WQL");
        const BstrScope wqlQuery(L"SELECT ProcessorId FROM Win32_Processor");
        if (!wqlLang.IsValid() || !wqlQuery.IsValid())
        {
            pSvc->Release();
            return "";
        }

        hr = pSvc->ExecQuery(wqlLang.Get(), wqlQuery.Get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
        if (FAILED(hr))
        {
            pSvc->Release();
            return "";
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
                id = BstrToUtf8(vt.bstrVal);
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
        logical = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

        // 先查询所需缓冲区大小（典型用法：第一次调用会失败，GetLastError=ERROR_INSUFFICIENT_BUFFER）
        DWORD returnedLength = 0;
        const BOOL firstCallOk = ::GetLogicalProcessorInformationEx(RelationAll, nullptr, &returnedLength);
        if (firstCallOk || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || returnedLength == 0)
        {
            cores = packages = 0;
            // NUMA 回退
            ULONG highest = 0;
            if (::GetNumaHighestNodeNumber(&highest))
            {
                numa = highest + 1;
            }
            else
            {
                numa = 1;
            }
            return;
        }

        std::vector<uint8_t> buffer(returnedLength);
        if (!::GetLogicalProcessorInformationEx(
            RelationAll,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &returnedLength))
        {
            cores = packages = 0;
            // NUMA 回退
            ULONG highest = 0;
            if (::GetNumaHighestNodeNumber(&highest))
            {
                numa = highest + 1;
            }
            else
            {
                numa = 1;
            }
            return;
        }

        cores = 0;
        packages = 0;
        numa = 0;

        uint8_t* ptr = buffer.data();
        uint8_t* end = buffer.data() + returnedLength;

        while (ptr < end)
        {
            auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
            if (info->Size == 0)
            {
                break;
            }

            if (ptr + info->Size > end)
            {
                break;
            }

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
            numa = ::GetNumaHighestNodeNumber(&highest) ? (highest + 1) : 1;
        }
    }

    // CallNtPowerInformation(ProcessorInformation) 的输出布局与 PROCESSOR_POWER_INFORMATION 等价；
    // 为避免与不同 SDK 版本的类型定义冲突，这里使用独立的本地结构体。
    struct GbProcessorPowerInformation
    {
        ULONG number;
        ULONG maxMhz;
        ULONG currentMhz;
        ULONG mhzLimit;
        ULONG maxIdleState;
        ULONG currentIdleState;
    };

    static_assert(sizeof(GbProcessorPowerInformation) == sizeof(ULONG) * 6, "GbProcessorPowerInformation size mismatch.");

    // 通过 CallNtPowerInformation 拿频率（MHz）
    static void QueryWindowsFrequencies(uint64_t& baseHz, uint64_t& maxHz)
    {
        baseHz = maxHz = 0;

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);

        DWORD n = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (n == 0)
        {
            n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 64;
        }

        vector<GbProcessorPowerInformation> ppi(n);
        const ULONG outSize = static_cast<ULONG>(sizeof(GbProcessorPowerInformation) * ppi.size());
        const NTSTATUS st = CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), outSize);
        if (st != 0)
        {
            return;
        }

        uint32_t baseMhzObserved = 0;
        uint32_t maxMhzObserved = 0;
        for (size_t i = 0; i < ppi.size(); ++i)
        {
            baseMhzObserved = max((uint32_t)baseMhzObserved, (uint32_t)ppi[i].maxMhz);
            maxMhzObserved = max((uint32_t)maxMhzObserved, max((uint32_t)ppi[i].maxMhz, (uint32_t)ppi[i].mhzLimit));
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
        std::memcpy(ven + 0, &regs[1], sizeof(int)); // EBX
        std::memcpy(ven + 4, &regs[3], sizeof(int)); // EDX
        std::memcpy(ven + 8, &regs[2], sizeof(int)); // ECX
        vendor = ven;

        int maxExt[4] = { 0 };
        __cpuid(maxExt, 0x80000000);
        if (static_cast<unsigned>(maxExt[0]) >= 0x80000004)
        {
            char buf[49] = { 0 };

            int extRegs[4] = { 0 };
            __cpuid(extRegs, 0x80000002);
            std::memcpy(buf + 0, extRegs, sizeof(extRegs));

            __cpuid(extRegs, 0x80000003);
            std::memcpy(buf + 16, extRegs, sizeof(extRegs));

            __cpuid(extRegs, 0x80000004);
            std::memcpy(buf + 32, extRegs, sizeof(extRegs));

            brand = Trim(string(buf));
        }

        int leaf1[4] = { 0 };
        __cpuid(leaf1, 1);

        // 超级管理程序存在位：Leaf1 ECX bit31
        hypervisor = !!(leaf1[2] & (1 << 31));

        // 基本特性
        auto add = [&](bool cond, const char* name) {
            if (cond)
            {
                features.emplace_back(name);
            }
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

    static wstring Utf8ToWide(const string& s)
    {
        if (s.empty())
        {
            return wstring();
        }

        const int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0)
        {
            return wstring();
        }

        wstring ws(static_cast<size_t>(len), L'\0');
        const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &ws[0], len);
        if (written <= 0)
        {
            return wstring();
        }

        return ws;
    }

    static string WideToUtf8(const wstring& ws)
    {
        if (ws.empty())
        {
            return string();
        }

        const int len = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0)
        {
            return string();
        }

        string s(static_cast<size_t>(len), '\0');
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &s[0], len, nullptr, nullptr);
        if (written <= 0)
        {
            return string();
        }

        return s;
    }

    static string ExpandEnvironmentStringsUtf8(const string& valueUtf8)
    {
        if (valueUtf8.empty())
        {
            return valueUtf8;
        }

        const wstring srcW = Utf8ToWide(valueUtf8);
        if (srcW.empty())
        {
            return valueUtf8;
        }

        DWORD needed = ::ExpandEnvironmentStringsW(srcW.c_str(), nullptr, 0);
        if (needed == 0)
        {
            return valueUtf8;
        }

        // ExpandEnvironmentStringsW 的返回值包含结尾 NUL。
        // 环境变量可能在调用前后变化：若实际需要更大的缓冲区，则扩容后再重试一次。
        for (int attempt = 0; attempt < 2; attempt++)
        {
            wstring dstW(static_cast<size_t>(needed), L'\0');
            const DWORD written = ::ExpandEnvironmentStringsW(srcW.c_str(), &dstW[0], needed);
            if (written == 0)
            {
                return valueUtf8;
            }

            if (written > needed)
            {
                needed = written;
                continue;
            }

            // written 含结尾 NUL
            dstW.resize(wcslen(dstW.c_str()));
            return WideToUtf8(dstW);
        }

        return valueUtf8;
    }

    static unordered_map<string, string> ReadEnvVarsFromRegistryPath(const string& configPathUtf8)
    {
        unordered_map<string, string> result;

        GB_ConfigItem configItem;
        if (!GB_GetConfigItem(configPathUtf8, configItem, false))
        {
            return result;
        }

        for (const GB_ConfigValue& value : configItem.values)
        {
            if (value.nameUtf8.empty())
            {
                // 环境变量不关心默认值
                continue;
            }

            switch (value.valueType)
            {
            case GB_ConfigValueType::GbConfigValueType_String:
                result[value.nameUtf8] = value.valueUtf8;
                break;
            case GB_ConfigValueType::GbConfigValueType_ExpandString:
                result[value.nameUtf8] = ExpandEnvironmentStringsUtf8(value.valueUtf8);
                break;
            case GB_ConfigValueType::GbConfigValueType_MultiString:
            {
                // 罕见情况：多字符串环境变量，按 ';' 拼接（PATH 语义更贴近）
                string joined;
                for (size_t i = 0; i < value.multiStringValuesUtf8.size(); i++)
                {
                    if (i > 0)
                    {
                        joined.push_back(';');
                    }
                    joined += value.multiStringValuesUtf8[i];
                }
                result[value.nameUtf8] = joined;
                break;
            }
            case GB_ConfigValueType::GbConfigValueType_DWord:
                result[value.nameUtf8] = to_string(value.dwordValue);
                break;
            case GB_ConfigValueType::GbConfigValueType_QWord:
                result[value.nameUtf8] = to_string(value.qwordValue);
                break;
            default:
                // 其他类型不是“环境变量”的常见形态，直接跳过
                break;
            }
        }

        return result;
    }

    // 直接从注册表枚举环境变量（作为 GB_Config 读取失败时的兜底）。
    // - 对 REG_EXPAND_SZ 做 ExpandEnvironmentStrings 展开（与 ReadEnvVarsFromRegistryPath 行为一致）。
    // - REG_MULTI_SZ 用 ';' 拼接（PATH 语义更贴近）。
    // - 其它常见整数类型（REG_DWORD/REG_QWORD）转为十进制字符串。
    static unordered_map<string, string> ReadEnvVarsFromRegistryKey(HKEY rootKey, const wchar_t* subKey)
    {
        unordered_map<string, string> result;

        if (subKey == nullptr)
        {
            return result;
        }

        HKEY keyHandle = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKey, 0, KEY_READ, &keyHandle);
        if (openResult != ERROR_SUCCESS)
        {
            return result;
        }

        DWORD valueCount = 0;
        DWORD maxValueNameLen = 0;
        DWORD maxValueDataLen = 0;
        const LONG infoResult = ::RegQueryInfoKeyW(
            keyHandle,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            &valueCount,
            &maxValueNameLen,
            &maxValueDataLen,
            nullptr,
            nullptr);

        if (infoResult != ERROR_SUCCESS)
        {
            ::RegCloseKey(keyHandle);
            return result;
        }

        std::vector<wchar_t> valueNameBuffer(static_cast<size_t>(maxValueNameLen) + 1u, L'\0');
        std::vector<BYTE> dataBuffer(static_cast<size_t>(maxValueDataLen) + sizeof(wchar_t), 0);

        for (DWORD valueIndex = 0; valueIndex < valueCount; valueIndex++)
        {
            DWORD valueNameLen = maxValueNameLen + 1;
            DWORD type = 0;
            DWORD dataSize = maxValueDataLen;

            LONG enumResult = ::RegEnumValueW(
                keyHandle,
                valueIndex,
                valueNameBuffer.data(),
                &valueNameLen,
                nullptr,
                &type,
                dataBuffer.data(),
                &dataSize);

            if (enumResult != ERROR_SUCCESS)
            {
                continue;
            }

            if (valueNameLen == 0)
            {
                // 默认值（无名）不视为环境变量
                continue;
            }

            const std::wstring valueNameW(valueNameBuffer.data(), valueNameLen);
            const std::string valueNameUtf8 = WideToUtf8(valueNameW);
            if (valueNameUtf8.empty())
            {
                continue;
            }

            auto ReadStringValue = [&](std::wstring& outValueW) -> bool {
                const size_t wcharCount = (dataSize / sizeof(wchar_t));
                if (wcharCount == 0)
                {
                    outValueW.clear();
                    return true;
                }

                const wchar_t* dataWchars = reinterpret_cast<const wchar_t*>(dataBuffer.data());
                size_t len = 0;
                while (len < wcharCount && dataWchars[len] != L'\0')
                {
                    len++;
                }
                outValueW.assign(dataWchars, dataWchars + len);
                return true;
                };

            auto ReadMultiSzValue = [&](std::wstring& outValueW) -> bool {
                const size_t wcharCount = (dataSize / sizeof(wchar_t));
                if (wcharCount == 0)
                {
                    outValueW.clear();
                    return true;
                }

                const wchar_t* dataWchars = reinterpret_cast<const wchar_t*>(dataBuffer.data());
                std::wstring joined;

                size_t wcharIndex = 0;
                while (wcharIndex < wcharCount && dataWchars[wcharIndex] != L'\0')
                {
                    const size_t start = wcharIndex;
                    while (wcharIndex < wcharCount && dataWchars[wcharIndex] != L'\0')
                    {
                        wcharIndex++;
                    }

                    const size_t itemLen = wcharIndex - start;
                    if (itemLen > 0)
                    {
                        if (!joined.empty())
                        {
                            joined.push_back(L';');
                        }
                        joined.append(dataWchars + start, itemLen);
                    }

                    wcharIndex++; // skip NUL
                }

                outValueW = joined;
                return true;
                };

            if (type == REG_SZ || type == REG_EXPAND_SZ)
            {
                std::wstring valueW;
                if (!ReadStringValue(valueW))
                {
                    continue;
                }

                std::string valueUtf8 = WideToUtf8(valueW);
                if (type == REG_EXPAND_SZ)
                {
                    valueUtf8 = ExpandEnvironmentStringsUtf8(valueUtf8);
                }
                result[valueNameUtf8] = valueUtf8;
            }
            else if (type == REG_MULTI_SZ)
            {
                std::wstring valueW;
                if (!ReadMultiSzValue(valueW))
                {
                    continue;
                }

                result[valueNameUtf8] = WideToUtf8(valueW);
            }
            else if (type == REG_DWORD && dataSize >= sizeof(DWORD))
            {
                DWORD v = 0;
                std::memcpy(&v, dataBuffer.data(), sizeof(DWORD));
                result[valueNameUtf8] = std::to_string(static_cast<unsigned long long>(v));
            }
            else if (type == REG_QWORD && dataSize >= sizeof(unsigned long long))
            {
                unsigned long long v = 0;
                std::memcpy(&v, dataBuffer.data(), sizeof(unsigned long long));
                result[valueNameUtf8] = std::to_string(v);
            }
        }

        ::RegCloseKey(keyHandle);
        return result;
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

        bool flagsParsed = false;

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
            else if ((k == "flags" || k == "Features") && !flagsParsed)
            {
                istringstream iss(v);
                string f;
                while (iss >> f)
                {
                    info.flags.push_back(f);
                }
                flagsParsed = true;
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

                int pkgId = -1;
                int coreId = -1;
                string s;

                if (ReadFirstLine(pathPkg, s))
                {
                    char* endp = nullptr;
                    const long v = strtol(s.c_str(), &endp, 10);
                    if (endp != s.c_str())
                    {
                        pkgId = static_cast<int>(v);
                    }
                }
                if (ReadFirstLine(pathCore, s))
                {
                    char* endp = nullptr;
                    const long v = strtol(s.c_str(), &endp, 10);
                    if (endp != s.c_str())
                    {
                        coreId = static_cast<int>(v);
                    }
                }

                if (pkgId >= 0)
                {
                    packageSet.insert(pkgId);
                }
                if (pkgId >= 0 && coreId >= 0)
                {
                    coreSet.insert({ pkgId, coreId });
                }
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
            return string();
        }

        const UINT wcharCount = ::SysStringLen(b); // 不含结尾 NUL
        if (wcharCount == 0)
        {
            return string();
        }

        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, b, static_cast<int>(wcharCount), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
        {
            return string();
        }

        string out(static_cast<size_t>(needed), '\0');
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, b, static_cast<int>(wcharCount), &out[0], needed, nullptr, nullptr);
        if (written <= 0)
        {
            return string();
        }

        return out;
    }

    // 执行 WQL 并取首条记录里若干字符串属性（不存在则留空）
    static bool WmiQueryFirstRowStrings(const wchar_t* wql, const vector<wstring>& props, vector<string>& outStrings)
    {
        outStrings.clear();

        ComScope com;
        if (!com.IsOk())
        {
            return false;
        }

        IWbemLocator* pLoc = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            return false;
        }

        IWbemServices* pSvc = nullptr;
        const BstrScope rootPath(L"ROOT\\CIMV2");
        if (!rootPath.IsValid())
        {
            pLoc->Release();
            return false;
        }

        HRESULT hr = pLoc->ConnectServer(rootPath.Get(), nullptr, nullptr, 0, 0, 0, 0, &pSvc);
        pLoc->Release();
        if (FAILED(hr))
        {
            return false;
        }

        (void)CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

        IEnumWbemClassObject* pEnum = nullptr;
        const BstrScope wqlLang(L"WQL");
        const BstrScope wqlQuery(wql);
        if (!wqlLang.IsValid() || !wqlQuery.IsValid())
        {
            pSvc->Release();
            return false;
        }

        hr = pSvc->ExecQuery(wqlLang.Get(), wqlQuery.Get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
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
                        for (LONG arrayIndex = l; arrayIndex <= u; arrayIndex++)
                        {
                            BSTR bs = nullptr;
                            if (SUCCEEDED(SafeArrayGetElement(psa, &arrayIndex, &bs)) && bs)
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
        auto parseFile = [&](const string& path) -> bool {
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
                if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front())
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

string GB_CpuInfo::Serialize() const
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

GB_CpuInfo GB_GetCpuInfo()
{
    GB_CpuInfo info;
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

string GB_MotherboardInfo::Serialize() const
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

GB_MotherboardInfo GB_GetMotherboardInfo()
{
    GB_MotherboardInfo info;

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

string GB_OsInfo::Serialize() const
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

string GB_GenerateHardwareId()
{
    const GB_CpuInfo cpuInfo = GB_GetCpuInfo();
    const GB_MotherboardInfo motherboardInfo = GB_GetMotherboardInfo();

    const string cpuId = cpuInfo.cpuSerial.empty() ? cpuInfo.processorId : cpuInfo.cpuSerial;
    const string motherboardId = motherboardInfo.serialNumber.empty() ? motherboardInfo.uuid : motherboardInfo.serialNumber;
    const string hardwareId = cpuId + "--" + motherboardId;
    return GB_GetSha256(hardwareId);
}

// ===== 3) GB_GetOsInfo() 主体 =====
GB_OsInfo GB_GetOsInfo()
{
    GB_OsInfo info;
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

// ============================================================================
// Windows 环境变量操作（GB_WindowsEnvVarOperator）
// ============================================================================

namespace internal
{
#if defined(_WIN32)

    static std::string ToLowerAscii(const std::string& s)
    {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    static std::wstring TrimQuotesAndSpaces(const std::wstring& s)
    {
        size_t beginIndex = 0;
        size_t endIndex = s.size();

        while (beginIndex < endIndex)
        {
            const wchar_t ch = s[beginIndex];
            if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n')
            {
                beginIndex++;
                continue;
            }
            break;
        }

        while (endIndex > beginIndex)
        {
            const wchar_t ch = s[endIndex - 1];
            if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n')
            {
                endIndex--;
                continue;
            }
            break;
        }

        std::wstring result = s.substr(beginIndex, endIndex - beginIndex);

        // 去掉外层引号
        if (result.size() >= 2)
        {
            if ((result.front() == L'"' && result.back() == L'"') ||
                (result.front() == L'\'' && result.back() == L'\''))
            {
                result = result.substr(1, result.size() - 2);
            }
        }

        return result;
    }

    static std::wstring NormalizePathEntryForCompare(const std::wstring& entryW)
    {
        std::wstring result = TrimQuotesAndSpaces(entryW);

        // 统一分隔符到 '\'
        std::replace(result.begin(), result.end(), L'/', L'\\');

        // 去掉末尾 '\'（根路径除外）
        while (result.size() > 1 && (result.back() == L'\\' || result.back() == L'/'))
        {
            if (result.size() == 3 && result[1] == L':' && (result[2] == L'\\' || result[2] == L'/'))
            {
                break; // "C:\"
            }
            result.pop_back();
        }

        // 小写化（Windows 路径比较通常大小写不敏感）
        if (!result.empty())
        {
            ::CharLowerBuffW(&result[0], static_cast<DWORD>(result.size()));
        }

        return result;
    }

    static bool BroadcastEnvironmentChange()
    {
        DWORD_PTR result = 0;
        const wchar_t* env = L"Environment";
        const LRESULT r = ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(env), SMTO_ABORTIFHUNG, 5000, &result);
        return r != 0;
    }

    static bool ReadRegistryStringValue(HKEY rootKey, const wchar_t* subKey, const std::wstring& valueNameW, std::wstring* outValueW, DWORD* outType)
    {
        if (outValueW == nullptr || outType == nullptr)
        {
            return false;
        }

        outValueW->clear();
        *outType = 0;

        HKEY keyHandle = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKey, 0, KEY_READ, &keyHandle);
        if (openResult != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD type = 0;
        DWORD bytes = 0;
        LONG queryResult = ::RegQueryValueExW(keyHandle, valueNameW.c_str(), nullptr, &type, nullptr, &bytes);
        if (queryResult != ERROR_SUCCESS)
        {
            ::RegCloseKey(keyHandle);
            return false;
        }

        if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
        {
            ::RegCloseKey(keyHandle);
            return false;
        }

        // 使用 wchar_t 缓冲区避免对 uint8_t 缓冲区做 reinterpret_cast 可能导致的非对齐访问问题
        const size_t wcharCount = (bytes / sizeof(wchar_t)) + 1; // +1 保证可 NUL 结尾
        std::vector<wchar_t> buffer(wcharCount, L'\0');

        DWORD bytesToRead = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        queryResult = ::RegQueryValueExW(
            keyHandle,
            valueNameW.c_str(),
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(buffer.data()),
            &bytesToRead);

        ::RegCloseKey(keyHandle);

        if (queryResult != ERROR_SUCCESS)
        {
            return false;
        }

        *outType = type;
        buffer.back() = L'\0';

        if (type == REG_SZ || type == REG_EXPAND_SZ)
        {
            *outValueW = std::wstring(buffer.data());
            return true;
        }

        // REG_MULTI_SZ：以 '\0' 分隔的字符串序列，末尾以双 '\0' 结束
        std::wstring joined;
        size_t i = 0;
        while (i < buffer.size() && buffer[i] != L'\0')
        {
            const wchar_t* itemStart = &buffer[i];

            size_t itemLen = 0;
            while ((i + itemLen) < buffer.size() && itemStart[itemLen] != L'\0')
            {
                itemLen++;
            }

            if (itemLen > 0)
            {
                if (!joined.empty())
                {
                    joined.push_back(L';');
                }
                joined.append(itemStart, itemLen);
            }

            i += itemLen + 1;
        }

        *outValueW = joined;
        return true;
    }

    static bool WriteRegistryStringValue(HKEY rootKey, const wchar_t* subKey, const std::wstring& valueNameW, const std::wstring& valueW, DWORD type)
    {
        HKEY keyHandle = nullptr;
        DWORD disposition = 0;
        const LONG createResult = ::RegCreateKeyExW(rootKey, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &keyHandle, &disposition);
        if (createResult != ERROR_SUCCESS)
        {
            return false;
        }

        const DWORD bytes = static_cast<DWORD>((valueW.size() + 1) * sizeof(wchar_t));
        const LONG setResult = ::RegSetValueExW(keyHandle, valueNameW.c_str(), 0, type, reinterpret_cast<const BYTE*>(valueW.c_str()), bytes);

        ::RegCloseKey(keyHandle);
        return setResult == ERROR_SUCCESS;
    }

    static bool DeleteRegistryValue(HKEY rootKey, const wchar_t* subKey, const std::wstring& valueNameW)
    {
        HKEY keyHandle = nullptr;
        const LONG openResult = ::RegOpenKeyExW(rootKey, subKey, 0, KEY_SET_VALUE, &keyHandle);
        if (openResult != ERROR_SUCCESS)
        {
            return false;
        }

        const LONG delResult = ::RegDeleteValueW(keyHandle, valueNameW.c_str());
        ::RegCloseKey(keyHandle);

        return delResult == ERROR_SUCCESS || delResult == ERROR_FILE_NOT_FOUND;
    }

    static std::vector<std::string> SplitPathList(const std::string& pathValueUtf8)
    {
        std::vector<std::string> entries;
        std::string current;

        for (size_t i = 0; i < pathValueUtf8.size(); i++)
        {
            const char ch = pathValueUtf8[i];
            if (ch == ';')
            {
                const std::string trimmed = Trim(current);
                if (!trimmed.empty())
                {
                    entries.push_back(trimmed);
                }
                current.clear();
                continue;
            }
            current.push_back(ch);
        }

        const std::string trimmed = Trim(current);
        if (!trimmed.empty())
        {
            entries.push_back(trimmed);
        }

        return entries;
    }

    static std::string JoinPathList(const std::vector<std::string>& entries)
    {
        std::string joined;
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (entries[i].empty())
            {
                continue;
            }

            if (!joined.empty())
            {
                joined.push_back(';');
            }
            joined += entries[i];
        }
        return joined;
    }

    static bool ReadRawEnvVarFromRegistry(HKEY rootKey, const wchar_t* subKey, const std::string& varNameUtf8, std::string* outValueUtf8, DWORD* outType)
    {
        if (outValueUtf8 == nullptr || outType == nullptr)
        {
            return false;
        }

        const std::wstring varNameW = Utf8ToWide(varNameUtf8);
        if (varNameW.empty())
        {
            return false;
        }

        std::wstring valueW;
        DWORD type = 0;
        if (!ReadRegistryStringValue(rootKey, subKey, varNameW, &valueW, &type))
        {
            // 再试一次常见大小写（PATH）
            if (ToLowerAscii(varNameUtf8) == "path")
            {
                if (!ReadRegistryStringValue(rootKey, subKey, L"Path", &valueW, &type) &&
                    !ReadRegistryStringValue(rootKey, subKey, L"PATH", &valueW, &type))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }

        *outType = type;
        *outValueUtf8 = WideToUtf8(valueW);
        return true;
    }

    static bool WriteEnvVarToRegistry(HKEY rootKey, const wchar_t* subKey, const std::string& varNameUtf8, const std::string& valueUtf8)
    {
        const std::wstring varNameW = Utf8ToWide(varNameUtf8);
        const std::wstring valueW = Utf8ToWide(valueUtf8);
        if (varNameW.empty())
        {
            return false;
        }

        // valueUtf8 非空但转换失败：避免把“非法 UTF-8”误写成空值
        if (!valueUtf8.empty() && valueW.empty())
        {
            return false;
        }

        // 尽量保留既有类型
        DWORD existingType = 0;
        std::wstring existingValueW;
        DWORD writeType = REG_SZ;
        if (ReadRegistryStringValue(rootKey, subKey, varNameW, &existingValueW, &existingType))
        {
            if (existingType == REG_EXPAND_SZ || existingType == REG_SZ)
            {
                writeType = existingType;
            }
        }
        else
        {
            // 新建：根据是否包含 %...% 推断
            if (valueW.find(L'%') != std::wstring::npos)
            {
                writeType = REG_EXPAND_SZ;
            }
        }

        if (!WriteRegistryStringValue(rootKey, subKey, varNameW, valueW, writeType))
        {
            return false;
        }

        // 更新当前进程的环境块（仅影响当前进程及其子进程）
        ::SetEnvironmentVariableW(varNameW.c_str(), valueW.c_str());

        // 通知系统刷新
        BroadcastEnvironmentChange();
        return true;
    }

#endif // _WIN32
} // namespace internal


// -------------------- UserEnvVarOperator --------------------

std::unordered_map<std::string, std::string> GB_WindowsEnvVarOperator::UserEnvVarOperator::GetAllUserEnvironmentVariables()
{
#if defined(_WIN32)
    static const std::string configPathUtf8 = GB_STR("计算机\\HKEY_CURRENT_USER\\Environment");
    std::unordered_map<std::string, std::string> result = internal::ReadEnvVarsFromRegistryPath(configPathUtf8);
    if (!result.empty())
    {
        return result;
    }

    // 兜底：直接枚举注册表。
    return internal::ReadEnvVarsFromRegistryKey(HKEY_CURRENT_USER, L"Environment");
#else
    return std::unordered_map<std::string, std::string>();
#endif
}

bool GB_WindowsEnvVarOperator::UserEnvVarOperator::GetUserEnvironmentVariable(const std::string& varNameUtf8, std::string* outValueUtf8)
{
    if (outValueUtf8 == nullptr || varNameUtf8.empty())
    {
        return false;
    }

#if defined(_WIN32)
    std::string valueUtf8;
    DWORD type = 0;
    if (!internal::ReadRawEnvVarFromRegistry(HKEY_CURRENT_USER, L"Environment", varNameUtf8, &valueUtf8, &type))
    {
        outValueUtf8->clear();
        return false;
    }

    // 与 GetAllUserEnvironmentVariables() 行为保持一致：REG_EXPAND_SZ 进行展开
    if (type == REG_EXPAND_SZ)
    {
        valueUtf8 = internal::ExpandEnvironmentStringsUtf8(valueUtf8);
    }

    *outValueUtf8 = valueUtf8;
    return true;
#else
    (void)varNameUtf8;
    outValueUtf8->clear();
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::UserEnvVarOperator::SetUserEnvironmentVariable(const std::string& varNameUtf8, const std::string& valueUtf8)
{
#if defined(_WIN32)
    if (varNameUtf8.empty())
    {
        return false;
    }

    const wchar_t* subKey = L"Environment";
    return internal::WriteEnvVarToRegistry(HKEY_CURRENT_USER, subKey, varNameUtf8, valueUtf8);
#else
    (void)varNameUtf8;
    (void)valueUtf8;
    return false;
#endif
}


// -------------------- UserPathOperator --------------------

std::vector<std::string> GB_WindowsEnvVarOperator::UserEnvVarOperator::UserPathOperator::GetUserPathEntries()
{
#if defined(_WIN32)
    std::string pathValueUtf8;
    DWORD type = 0;
    if (!internal::ReadRawEnvVarFromRegistry(HKEY_CURRENT_USER, L"Environment", "Path", &pathValueUtf8, &type))
    {
        return std::vector<std::string>();
    }

    return internal::SplitPathList(pathValueUtf8);
#else
    return std::vector<std::string>();
#endif
}

bool GB_WindowsEnvVarOperator::UserEnvVarOperator::UserPathOperator::HasUserPathEntry(const std::string& entryUtf8)
{
#if defined(_WIN32)
    if (entryUtf8.empty())
    {
        return false;
    }

    const std::wstring entryW = internal::Utf8ToWide(entryUtf8);
    const std::wstring entryNormW = internal::NormalizePathEntryForCompare(entryW);

    const std::vector<std::string> entries = GetUserPathEntries();
    for (const std::string& e : entries)
    {
        const std::wstring eW = internal::Utf8ToWide(e);
        const std::wstring eNormW = internal::NormalizePathEntryForCompare(eW);
        if (!entryNormW.empty() && entryNormW == eNormW)
        {
            return true;
        }

        // 兼容：展开后比较（处理 %SystemRoot% 等）
        const std::wstring entryExpandedW = internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(entryUtf8));
        const std::wstring eExpandedW = internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(e));
        if (!entryExpandedW.empty() && !eExpandedW.empty())
        {
            if (internal::NormalizePathEntryForCompare(entryExpandedW) == internal::NormalizePathEntryForCompare(eExpandedW))
            {
                return true;
            }
        }
    }

    return false;
#else
    (void)entryUtf8;
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::UserEnvVarOperator::UserPathOperator::AddUserPathEntry(const std::string& entryUtf8, bool append)
{
#if defined(_WIN32)
    const std::string entryTrimmed = internal::Trim(entryUtf8);
    if (entryTrimmed.empty())
    {
        return false;
    }

    if (HasUserPathEntry(entryTrimmed))
    {
        return true;
    }

    std::vector<std::string> entries = GetUserPathEntries();
    if (append)
    {
        entries.push_back(entryTrimmed);
    }
    else
    {
        entries.insert(entries.begin(), entryTrimmed);
    }

    const std::string joined = internal::JoinPathList(entries);
    return GB_WindowsEnvVarOperator::UserEnvVarOperator::SetUserEnvironmentVariable("Path", joined);
#else
    (void)entryUtf8;
    (void)append;
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::UserEnvVarOperator::UserPathOperator::RemoveUserPathEntry(const std::string& entryUtf8)
{
#if defined(_WIN32)
    const std::string entryTrimmed = internal::Trim(entryUtf8);
    if (entryTrimmed.empty())
    {
        return false;
    }

    std::vector<std::string> entries = GetUserPathEntries();
    if (entries.empty())
    {
        return true;
    }

    const std::wstring entryNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(entryTrimmed));
    const std::wstring entryExpandedNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(entryTrimmed)));

    std::vector<std::string> filtered;
    filtered.reserve(entries.size());

    for (const std::string& e : entries)
    {
        const std::wstring eNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(e));
        if (!entryNormW.empty() && entryNormW == eNormW)
        {
            continue;
        }

        const std::wstring eExpandedNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(e)));
        if (!entryExpandedNormW.empty() && entryExpandedNormW == eExpandedNormW)
        {
            continue;
        }

        filtered.push_back(e);
    }

    const std::string joined = internal::JoinPathList(filtered);
    if (joined.empty())
    {
        // 没有条目时，删除用户 Path 变量更贴近“未设置用户 Path”
        if (!internal::DeleteRegistryValue(HKEY_CURRENT_USER, L"Environment", L"Path"))
        {
            return false;
        }

        internal::BroadcastEnvironmentChange();
        ::SetEnvironmentVariableW(L"Path", nullptr);
        return true;
    }

    return GB_WindowsEnvVarOperator::UserEnvVarOperator::SetUserEnvironmentVariable("Path", joined);
#else
    (void)entryUtf8;
    return false;
#endif
}

std::unordered_map<std::string, std::string> GB_WindowsEnvVarOperator::SystemEnvVarOperator::GetAllSystemEnvironmentVariables()
{
#if defined(_WIN32)
    static const std::string configPathUtf8 = GB_STR("计算机\\HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    std::unordered_map<std::string, std::string> result = internal::ReadEnvVarsFromRegistryPath(configPathUtf8);
    if (!result.empty())
    {
        return result;
    }

    // 兜底：直接枚举注册表。
    return internal::ReadEnvVarsFromRegistryKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
#else
    return std::unordered_map<std::string, std::string>();
#endif
}

bool GB_WindowsEnvVarOperator::SystemEnvVarOperator::GetSystemEnvironmentVariable(const std::string& varNameUtf8, std::string* outValueUtf8)
{
    if (outValueUtf8 == nullptr || varNameUtf8.empty())
    {
        return false;
    }

#if defined(_WIN32)
    std::string valueUtf8;
    DWORD type = 0;
    const wchar_t* subKey = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    if (!internal::ReadRawEnvVarFromRegistry(HKEY_LOCAL_MACHINE, subKey, varNameUtf8, &valueUtf8, &type))
    {
        outValueUtf8->clear();
        return false;
    }

    // 与 GetAllSystemEnvironmentVariables() 行为保持一致：REG_EXPAND_SZ 进行展开
    if (type == REG_EXPAND_SZ)
    {
        valueUtf8 = internal::ExpandEnvironmentStringsUtf8(valueUtf8);
    }

    *outValueUtf8 = valueUtf8;
    return true;
#else
    (void)varNameUtf8;
    outValueUtf8->clear();
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::SystemEnvVarOperator::SetSystemEnvironmentVariable(const std::string& varNameUtf8, const std::string& valueUtf8)
{
#if defined(_WIN32)
    if (varNameUtf8.empty())
    {
        return false;
    }

    const wchar_t* subKey = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    return internal::WriteEnvVarToRegistry(HKEY_LOCAL_MACHINE, subKey, varNameUtf8, valueUtf8);
#else
    (void)varNameUtf8;
    (void)valueUtf8;
    return false;
#endif
}

std::vector<std::string> GB_WindowsEnvVarOperator::SystemEnvVarOperator::SystemPathOperator::GetSystemPathEntries()
{
#if defined(_WIN32)
    std::string pathValueUtf8;
    DWORD type = 0;
    const wchar_t* subKey = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
    if (!internal::ReadRawEnvVarFromRegistry(HKEY_LOCAL_MACHINE, subKey, "Path", &pathValueUtf8, &type))
    {
        return std::vector<std::string>();
    }

    return internal::SplitPathList(pathValueUtf8);
#else
    return std::vector<std::string>();
#endif
}

bool GB_WindowsEnvVarOperator::SystemEnvVarOperator::SystemPathOperator::HasSystemPathEntry(const std::string& entryUtf8)
{
#if defined(_WIN32)
    if (entryUtf8.empty())
    {
        return false;
    }

    const std::wstring entryNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(entryUtf8));
    const std::vector<std::string> entries = GetSystemPathEntries();
    for (const std::string& e : entries)
    {
        const std::wstring eNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(e));
        if (!entryNormW.empty() && entryNormW == eNormW)
        {
            return true;
        }

        const std::wstring entryExpandedW = internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(entryUtf8));
        const std::wstring eExpandedW = internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(e));
        if (!entryExpandedW.empty() && !eExpandedW.empty())
        {
            if (internal::NormalizePathEntryForCompare(entryExpandedW) == internal::NormalizePathEntryForCompare(eExpandedW))
            {
                return true;
            }
        }
    }
    return false;
#else
    (void)entryUtf8;
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::SystemEnvVarOperator::SystemPathOperator::AddSystemPathEntry(const std::string& entryUtf8, bool append)
{
#if defined(_WIN32)
    const std::string entryTrimmed = internal::Trim(entryUtf8);
    if (entryTrimmed.empty())
    {
        return false;
    }

    if (HasSystemPathEntry(entryTrimmed))
    {
        return true;
    }

    std::vector<std::string> entries = GetSystemPathEntries();
    if (append)
    {
        entries.push_back(entryTrimmed);
    }
    else
    {
        entries.insert(entries.begin(), entryTrimmed);
    }

    const std::string joined = internal::JoinPathList(entries);
    return GB_WindowsEnvVarOperator::SystemEnvVarOperator::SetSystemEnvironmentVariable("Path", joined);
#else
    (void)entryUtf8;
    (void)append;
    return false;
#endif
}

bool GB_WindowsEnvVarOperator::SystemEnvVarOperator::SystemPathOperator::RemoveSystemPathEntry(const std::string& entryUtf8)
{
#if defined(_WIN32)
    const std::string entryTrimmed = internal::Trim(entryUtf8);
    if (entryTrimmed.empty())
    {
        return false;
    }

    std::vector<std::string> entries = GetSystemPathEntries();
    if (entries.empty())
    {
        return true;
    }

    const std::wstring entryNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(entryTrimmed));
    const std::wstring entryExpandedNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(entryTrimmed)));

    std::vector<std::string> filtered;
    filtered.reserve(entries.size());

    for (const std::string& e : entries)
    {
        const std::wstring eNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(e));
        if (!entryNormW.empty() && entryNormW == eNormW)
        {
            continue;
        }

        const std::wstring eExpandedNormW = internal::NormalizePathEntryForCompare(internal::Utf8ToWide(internal::ExpandEnvironmentStringsUtf8(e)));
        if (!entryExpandedNormW.empty() && entryExpandedNormW == eExpandedNormW)
        {
            continue;
        }

        filtered.push_back(e);
    }

    const std::string joined = internal::JoinPathList(filtered);
    if (joined.empty())
    {
        // 系统 Path 清空通常不合理，但这里仍提供能力：写空值。
        return GB_WindowsEnvVarOperator::SystemEnvVarOperator::SetSystemEnvironmentVariable("Path", "");
    }

    return GB_WindowsEnvVarOperator::SystemEnvVarOperator::SetSystemEnvironmentVariable("Path", joined);
#else
    (void)entryUtf8;
    return false;
#endif
}

