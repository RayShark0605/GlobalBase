# GlobalBase

> 一个偏“工程实用主义”的 C++11 基础工具库：**UTF-8 字符串、文件系统/路径、I/O、日志、计时器、线程池、读写锁、缓存、系统信息、进程工具、网络请求、加密模块**，以及一组轻量 2D 几何类型。  
> 适合：做小工具、做中间层、给大项目充当底层。

---

## ✨ 亮点

- **跨平台**：Windows + Linux（同一套 API，能跨的尽量跨；个别模块按平台做分支）。  
- **依赖关系及路径已整理好**：`GB_Crypto` / `GB_Network` 内部使用了 **libcurl 8.18.0、OpenSSL 3.4.0、zlib**，但项目已处理好编译/集成，通常无需你额外编译/安装这些库。  
- **Windows 延迟加载（Delay-Load）**：在 Windows 上，上述三方库的 `.dll` 被设置为 **延迟加载** —— **只有真正调用到网络/加密相关功能时才会触发加载**，让不相关功能的启动更干净。  
- **个人积累的“学习成果归档”**：这是作者在工作与学习中不断沉淀的工具集合，**不保证像权威三方库一样完美**，可能存在不足甚至 bug（欢迎 issue/PR）。  
- **持续改进**：作者会持续扩充与优化接口与实现。  
- **许可证**：MIT License ✅

---

## 📦 模块导览（按头文件）

> 说明：本库大量接口以 `std::string` 作为文本承载，**默认约定为 UTF-8**；涉及路径的接口通常也以 **UTF-8 路径**作为输入输出。

### 1) 基础类型
- **`GB_BaseTypes.h`**
  - `using GB_ByteBuffer = std::vector<unsigned char>;`
  - `GB_ClassMagicNumber`：用于类序列化/识别的魔数常量。

### 2) UTF-8 字符串与编码
- **`GB_Utf8String.h`**
  - UTF-8 构造：`GB_MakeUtf8String(...)`，以及便捷宏 `GB_STR(...)`、`GB_CHAR(...)` 等。
  - 编码互转：`GB_Utf8ToAnsi` / `GB_AnsiToUtf8`、`GB_WStringToUtf8` / `GB_Utf8ToWString`
  - UTF-8 处理：长度/取字符/子串（按 Unicode 码点计）、`Split/Find/Replace/Trim/StartsWith/EndsWith` 等。
  - **注意事项**：
    - POSIX 下 `Ansi/Utf8` 判定与互转可能依赖当前 `LC_CTYPE` locale；库内部可能会在首次调用时尝试 `setlocale(LC_CTYPE, "")`（它是进程级全局且非线程安全）。如需完全避免该副作用，可在编译时定义 `GB_DISABLE_POSIX_SETLOCALE_AUTO_INIT`。

### 3) 文件系统 / 路径
- **`GB_FileSystem.h`**
  - 存在性与类型：`GB_IsFileExists` / `GB_IsDirectoryExists` / `GB_IsDirectoryEmpty`
  - 目录/文件操作：`GB_CreateDirectory`、`GB_DeleteFile`、`GB_DeleteDirectory`、`GB_CopyFile`、`GB_CreateFileRecursive`
  - 文件大小：`GB_GetFileSizeByte/KB/MB/GB`
  - 路径拆解：`GB_GetFileName`、`GB_GetFileExt`、`GB_GetDirectoryPath`
  - 文件列表：`GB_GetFilesList`
  - 特殊目录：`GB_GetExeDirectory`、`GB_GetHomeDirectory`、`GB_GetDesktopDirectory`、`GB_GetDownloadsDirectory`、`GB_GetTempDirectory`
  - 路径计算与拼接：`GB_GetRelativePath`、`GB_JoinPath`
  - **内容猜测**：`GB_GuessFileExt(const GB_ByteBuffer&)`（基于 magic number / 容器特征推测 `.png`/`.mp4`/`.webp` 等）
  - **注意事项**：
    - Windows 输入支持 `/` 与 `\` 混用；**输出统一使用 `/`**。
    - `GB_JoinPath` 支持 `.` / `..` 进行 lexical 级别规范化（不解析符号链接）。

### 4) I/O（文本与二进制）
- **`GB_IO.h`**
  - `GB_WriteUtf8ToFile`：可追加写入，可选择新文件写 BOM
  - `GB_ReadFileToBinary` / `GB_WriteBinaryToFile`
  - `GB_ByteBufferIO`：LE（小端）序列化辅助：`AppendUInt32LE / ReadUInt32LE / AppendDoubleLE ...`

### 5) 日志
- **`GB_Logger.h`**
  - 日志级别：`GB_LogLevel`
  - 异步日志：`GB_Logger::GetInstance()` 内部队列 + 后台线程消费
  - 快捷宏：`GBLOG_INFO/DEBUG/WARNING/ERROR/FATAL`
  - 全局控制：`GB_SetLogEnabled`、`GB_SetLogToConsole`、`GB_GetLogFilterLevel` 等
  - 崩溃处理：`GB_InstallCrashHandlers` / `GB_RemoveCrashHandlers`

### 6) 计时器 / 性能测量
- **`GB_Timer.h`**
  - RFC3339/ISO8601 风格本地时间字符串：`GetLocalTimeStr(...)`
  - `GB_Timer`：计时器
  - `GB_ScopeTimer` + `GB_SCOPE_TIMER(...)`：RAII 作用域计时

### 7) 并发原语
- **`GB_ReadWriteLock.h`**
  - C++11 读写锁（`std::mutex + std::condition_variable`）
  - **写优先（writer-preference）**：避免写者饥饿；持续写压力下读者可能饥饿
  - 非递归；不支持读锁“原子升级”为写锁
  - RAII：`GB_ReadLockGuard` / `GB_WriteLockGuard`
- **`GB_ThreadPool.h`**
  - 线程池：支持有界队列（`maxQueueSize`）、任务统计、优雅停机
  - `Enqueue`（返回 `future`，可捕获异常）、`Post`（fire-and-forget）
  - `TryEnqueue/TryPost`（非阻塞）与 `EnqueueFor/PostFor`（带超时）
  - **注意事项**：
    - `Post` 类任务若抛异常且未被接住，默认可能触发 `std::terminate`；可设置 `UnhandledExceptionHandler`。

### 8) 缓存
- **`GB_DataCache.h`**
  - 通用数据缓存（类型擦除）：`shared_ptr<void>` + 自定义字节数
  - 淘汰策略：`Lru/Lfu/Fifo/Random`，支持 `maxBytes` 内存预算
  - 统计：命中/未命中/淘汰/更新等
  - **注意事项**：头文件层面未体现锁；若用于多线程场景，请在外层加锁或自行封装并发控制。

### 9) 配置
- **`GB_Config.h`**
  - 默认配置路径（形如 Windows 注册表风格路径字符串）：`GB_GetGbConfigPath` 及其读写/枚举
  - 通用配置项：路径存在性、创建、键值读写、子项枚举、类型化值（字符串/二进制/DWORD/QWORD/MultiString/ExpandString）
  - 适合做“轻量配置中心/注册表封装/平台配置抽象”。

### 10) 系统信息 / 硬件指纹 / 环境变量
- **`GB_SysInfo.h`**
  - `GB_GetCpuInfo` / `GB_GetMotherboardInfo` / `GB_GetOsInfo`
  - `GB_GenerateHardwareId`：基于 CPU+主板信息生成硬件 ID
  - Windows 环境变量：`GB_WindowsEnvVarOperator`（用户变量/系统变量，读写并广播 `WM_SETTINGCHANGE`）
  - **注意事项**：系统变量写入可能需要管理员权限；非 Windows 平台相关接口按约定返回空/false。

### 11) 进程
- **`GB_Process.h`**
  - 是否管理员：`GB_IsRunningAsAdmin`
  - 提权重启：`GB_EnsureRunningAsAdmin`（Windows 下可能 `ExitProcess(0)` 硬退出）
  - 进程枚举与信息：`GB_GetAllProcessesInfo` / `GB_GetProcessInfo` / `GB_FindProcessIdsByName`
  - 进程控制：`GB_StartProcess` / `GB_TerminateProcessById` / `GB_TerminateProcessesByName`
  - 导入/导出符号签名：`GB_GetExportedFunctionSignatures` / `GB_GetImportedFunctionSignatures`

### 12) 网络（libcurl）
- **`GB_Network.h`**
  - 网络连通性：`GB_CanConnectToInternet`
  - GET 请求：`GB_RequestUrlData`（返回 `GB_NetworkResponse`，body 为原始字节流）
  - 下载：`GB_DownloadFile` / `GB_DownloadFileToPath`（支持策略、可选传入原子指针反馈总大小/已下载大小）
  - 代理：`GB_NetworkProxySettings`（系统代理/自定义代理/禁用代理）
  - TLS：可配置证书校验与自定义 CA

### 13) 加密（OpenSSL + zlib）
- **`GB_Crypto.h`**
  - Base64：`GB_Base64Encode/Decode`（支持 URL-safe/no-padding 等）
  - Hash：`GB_Md5Hash`、`GB_ShaHash`、`GB_Crc32Hash`
  - Argon2：`GB_Argon2Hash/Verify`（带可选 salt）
  - AES：`GB_AesEncrypt/Decrypt`、Base64 版本、以及 `Packed` 便捷封装（可自动打包必要字段）
  - RSA：密钥生成、加解密、Base64 版本（支持 padding/hash 等选项）
  - ECC：密钥生成、加解密、Base64 版本（支持曲线/格式选项）
  - **注意事项**：
    - 这是“工程可用 + 学习沉淀”的实现集合，**不等同于经过安全审计的密码库**；在高安全场景使用前请自行评估与测试。

### 14) SMB（Windows）
- **`GB_SmbAccessor.h`**
  - Windows-only：测试 TCP 445、测试 SMB 连接、枚举共享、连接/断开共享等
  - 适合做内网共享探测、自动挂载等工具化需求。

### 15) 2D 几何类型（轻量）
- **`GB_GeometryInterface.h`**：`GB_SerializableClass`（字符串/二进制序列化协议）+ 类型 ID
- **`GB_Vector2d.h`**：二维向量（长度/归一化/点积/叉积/变换等）
- **`GB_Point2d.h`**：二维点（与向量运算/距离/变换等）
- **`GB_Matrix3x3.h`**：3×3 矩阵（2D 仿射变换友好）
- **`GB_Rectangle.h`**：AABB（minX/minY/maxX/maxY），交并包含/Expand/Buffer/面积周长等

---

## 🚀 快速开始（集成方式建议）

> 由于每个工程的组织方式不同，这里给“最稳妥的通用套路”：

1. 拉取源码（示例）
   ```bash
   git clone https://github.com/RayShark0605/GlobalBase.git
   ```
2. 在 Windows 下，直接打开 GlobalBase.sln 即可编译；在 Linux 下，在仓库目录中执行以下步骤即可编译：
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
3. 若你只使用与加密/网络无关的模块，在 Windows 下也不会触发对应三方 `.dll` 的加载。


---

## 🧪 示例代码

### 1) UTF-8 字符串与路径
```cpp
#include "GB_Utf8String.h"
#include "GB_FileSystem.h"
#include "GB_IO.h"
#include "GB_Utility.h"

int main()
{
    GB_SetConsoleEncodingToUtf8();

    const std::string leftPathUtf8 = GB_STR("C:\\Work\\Data\\");
    const std::string rightPathUtf8 = GB_STR("../中文测试/中文.png");

    const std::string fullPathUtf8 = GB_JoinPath(leftPathUtf8, rightPathUtf8);

    const bool isFileExists = GB_IsFileExists(fullPathUtf8);
    if (isFileExists)
    {
        const GB_ByteBuffer bytes = GB_ReadFileToBinary(fullPathUtf8);
        const std::string guessedExt = GB_GuessFileExt(bytes);
        (void)guessedExt;
    }

    return 0;
}
```

### 2) 线程池（future / fire-and-forget）
```cpp
#include "GB_ThreadPool.h"
#include <iostream>

static int ComputeSomething(int inputValue)
{
    if (inputValue < 0)
    {
        throw std::runtime_error("inputValue must be >= 0");
    }
    return inputValue * inputValue;
}

int main()
{
    GB_ThreadPool threadPool(4, 0); // 4 个 worker；0 表示无界队列

    std::vector<std::future<int>> futures;
    futures.reserve(10);
    for (int i = 0; i < 10; i++)
    {
        futures.emplace_back(threadPool.Enqueue(ComputeSomething, i));
    }

    long long totalSum = 0;
    for (std::future<int>& future : futures)
    {
        totalSum += future.get(); // 这里会等任务完成，并取回返回值/异常
    }
    std::cout << "totalSum = " << totalSum << std::endl;

    // 可选：显式等到“线程池完全空闲”
    threadPool.WaitIdle();

    return 0; // 析构会默认 Drain + Join
}
```

### 3) 网络 GET（返回原始字节流）
```cpp
#include "GB_Network.h"

int main()
{
    GB_NetworkRequestOptions options;
    options.followRedirects = true;
    options.totalTimeoutMs = 15000;

    const GB_NetworkResponse response = GB_RequestUrlData("https://example.com", options);
    if (response.ok)
    {
        // response.body 是原始字节流，可能不是 UTF-8 文本
    }
    return 0;
}
```

### 4) AES（Packed Base64 便捷封装）
```cpp
#include <iostream>
#include "GB_Crypto.h"
#include "GB_Utf8String.h"
#include "GB_Timer.h"

using namespace std;
int main(int argc, char* argv[])
{
    const std::string key = "05780/';][2w4105169632dcvgbmklpo";
    const std::string iv = "i40xmjhd7,s1873m";
    const std::string message = GB_STR("汉皇重色思倾国，御宇多年求不得。杨家有女初长成，养在深闺人未识。天生丽质难自弃，一朝选在君王侧。回眸一笑百媚生，六宫粉黛无颜色。");

    const size_t iterations = 200000;

    // 先做一次普通加密用于基本验证 / 生成用于单独解密测试的密文
    std::string extra = "";
    const std::string encryptedMessageBase64 = GB_AES::GB_AesEncryptToBase64(message, key, iv, GB_AES::GB_AesOptions(), extra);

    volatile size_t guardEnc = 0; // 防止优化掉结果
    auto encDuration = GB_Timer::Measure([&]() {
        for (size_t i = 0; i < iterations; ++i)
        {
            std::string out = GB_AES::GB_AesEncryptToBase64(message, key, iv, GB_AES::GB_AesOptions(), extra);
            guardEnc += out.size();
        }
        });

    volatile size_t guardDec = 0;
    auto decDuration = GB_Timer::Measure([&]() {
        for (size_t i = 0; i < iterations; ++i)
        {
            std::string decrypted;
            bool ok = GB_AES::GB_AesDecryptFromBase64(encryptedMessageBase64, key, iv, GB_AES::GB_AesOptions(), "", decrypted);
            if (ok)
            {
                guardDec += decrypted.size();
            }
            else
            {
                guardDec += 0xFF; // 标记解密失败也计入，避免被优化
            }
        }
        });

    // 汇总输出
    const int64_t encNs = encDuration.count();
    const int64_t decNs = decDuration.count();

    cout << "Iterations: " << iterations << endl;

    cout << "[Encrypt] total: " << GB_Timer::FormatNanoseconds(encNs)
        << ", avg: " << (static_cast<double>(encNs) / iterations / 1000.0) << " us"
        << ", throughput: " << (static_cast<double>(iterations) * 1e9 / encNs) << " ops/s"
        << ", guard: " << guardEnc
        << endl;

    cout << "[Decrypt] total: " << GB_Timer::FormatNanoseconds(decNs)
        << ", avg: " << (static_cast<double>(decNs) / iterations / 1000.0) << " us"
        << ", throughput: " << (static_cast<double>(iterations) * 1e9 / decNs) << " ops/s"
        << ", guard: " << guardDec
        << endl;

    return 0;
}
```

---

## ⚠️ 设计取向与已知注意点

- 这是一个“作者自用 + 学习沉淀”的工具库：**实用优先，但不承诺权威级完备性**。
- `GB_Crypto` / `GB_Network` 依赖三方库（libcurl/OpenSSL/zlib），项目已做集成；Windows 下为延迟加载。
- POSIX 下编码互转可能触发 locale 初始化（见 `GB_Utf8String.h` 的说明）。
- 读写锁为写优先实现；线程池的 `Post` 异常策略需留意（建议业务层捕获或设置 handler）。
- `GB_SmbAccessor` 与 Windows 环境变量操作是 Windows 特化模块；跨平台代码中建议按平台编译开关隔离使用。

---

## 🗺️ Roadmap（作者计划持续推进）

- 更完整的单元测试
- 几何模块扩充更多常用 2D/3D 基础类型与变换工具
- 更细粒度的错误码/诊断信息输出
- 更多的模块、更多的接口

---

## 🤝 贡献方式

欢迎 issue / PR。  
如果你在工程中踩到了坑（尤其是跨平台差异、编码、网络、加密边界条件），把复现方式写清楚会非常有帮助。

---

## 📄 License

MIT License

---

## 🙏 特别致谢

- ChatGPT 5.2 Thinking
- Gemini
- 以及 libcurl / OpenSSL / zlib 等优秀开源项目生态
