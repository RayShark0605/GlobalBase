#ifndef GLOBALBASE_FILESYSTEM_H_H
#define GLOBALBASE_FILESYSTEM_H_H

#include "GlobalBasePort.h"
#include <string>
#include <vector>

/**
 * @brief 判断给定 UTF-8 路径是否存在且为“常规文件”（非目录）。
 *
 * @param filePathUtf8 目标路径（UTF-8）。支持混合分隔符，内部自动处理。
 * @return true  文件存在且不是目录；
 * @return false 不存在、是目录，或底层检查失败（例如 Windows 上 UTF-8→UTF-16 转换失败）。
 *
 * @remarks Windows 使用 GetFileAttributesW 判断存在性与目录标志；Linux 使用 stat 并判定 S_ISREG。
 *          只做存在性判断，不打开文件。时间复杂度 O(1)。
 */
GLOBALBASE_PORT bool GB_IsFileExists(const std::string& filePathUtf8);

/**
 * @brief 判断给定 UTF-8 路径是否存在且为“目录”。
 *
 * @param dirPathUtf8 目录路径（UTF-8）。支持混合分隔符。
 * @return true  路径存在且为目录；
 * @return false 不存在、不是目录，或底层检查失败。
 *
 * @remarks Windows 依赖 GetFileAttributesW 的 FILE_ATTRIBUTE_DIRECTORY；Linux 使用 stat 并判定 S_ISDIR。
 */
GLOBALBASE_PORT bool GB_IsDirectoryExists(const std::string& dirPathUtf8);

/**
 * @brief 递归创建目录（逐级创建），等价于“mkdir -p”行为。
 *
 * @param dirPathUtf8 目标目录（UTF-8）。允许末尾带或不带分隔符；支持盘符与 UNC（例如 //server/share）。
 * @return true  全部级别创建成功或已存在；
 * @return false 任一层创建失败（如同名常规文件阻塞、权限不足等）。
 *
 * @post 不返回路径，仅执行创建。实现对 Windows 盘符根与 UNC 前缀做了专门处理。
 */
GLOBALBASE_PORT bool GB_CreateDirectory(const std::string& dirPathUtf8);

/**
 * @brief 判断目录是否“为空”（仅包含 "." 与 ".." 视为为空）。
 *
 * @param dirPathUtf8 目录路径（UTF-8）。
 * @return true  目录存在且为空；
 * @return false 目录不存在、非目录、遍历失败或目录包含任何条目。
 *
 * @remarks Windows 使用 FindFirstFileExW/FindNextFileW 遍历；Linux 使用 opendir/readdir。
 */
GLOBALBASE_PORT bool GB_IsEmptyDirectory(const std::string& dirPathUtf8);

/**
 * @brief 递归删除目录（包含其所有内容），最后删除该目录本身。
 *
 * @param dirPathUtf8 目录路径（UTF-8）。
 * @return true  删除成功；
 * @return false 任一步骤失败（如只读文件、权限不足、正在占用等）。
 *
 * @details 实现先递归清空子项，再删除空目录。Linux 下对符号链接使用 lstat 区分：链接本身按“文件”删除；
 *          Windows 下按目录属性递归。该函数不尝试提升权限，不进入“挂载点/重解析点”的特殊处理。
 */
GLOBALBASE_PORT bool GB_DeleteDirectory(const std::string& dirPathUtf8);

/**
 * @brief 删除单个常规文件。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return true  删除成功；
 * @return false 文件不存在、是目录或底层删除失败。
 *
 * @remarks Windows 使用 DeleteFileW；Linux 使用 unlink。
 */
GLOBALBASE_PORT bool GB_DeleteFile(const std::string& filePathUtf8);

/**
 * @brief 复制文件到目标路径（允许覆盖）。
 *
 * @param srcFilePathUtf8 源文件（UTF-8）。
 * @param dstFilePathUtf8 目标文件（UTF-8），若已存在将被覆盖。
 * @return true  复制成功；
 * @return false 任一端打开失败、读写错误或系统调用失败。
 *
 * @notes Windows 使用 CopyFileW（允许覆盖）；Linux 使用标准 C I/O 流式复制（1MB 缓冲），
 *        不保证保留权限、时间戳、ACL 或扩展属性；不处理稀疏文件/洞洞文件、备用数据流等高级特性。
 */
GLOBALBASE_PORT bool GB_CopyFile(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8);

/**
 * @brief 列出目录下所有“文件”的完整路径（不含目录），可选递归。
 *
 * @param dirPathUtf8 目录路径（UTF-8）。
 * @param recursive   是否递归子目录。
 * @return std::vector<std::string>  文件路径列表；若目录不存在或遍历失败返回空。
 *
 * @remarks 返回路径统一使用正斜杠“/”，且不以“/”结尾。遍历期间遇到不可读目录/条目会被跳过。
 */
GLOBALBASE_PORT std::vector<std::string> GB_GetFilesList(const std::string& dirPathUtf8, bool recursive = false);

/**
 * @brief 获取文件名（可选是否保留扩展名）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8），允许混合分隔符。
 * @param withExt      true 返回包含扩展名的文件名；false 返回去掉“最后一个点”之后扩展的文件名。
 * @return std::string 文件名（不含路径）。特殊情况：以点开头的隐藏文件（如 ".bashrc"），
 *         当 withExt=false 时将返回空串（因为首个字符即为“最后一个点”）。
 *
 * @remarks 内部将反斜杠标准化为“/”后再截取。
 */
GLOBALBASE_PORT std::string GB_GetFileName(const std::string& filePathUtf8, bool withExt = false);

/**
 * @brief 获取“最后一个点”起算的扩展名（包含前导点）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8）。
 * @return std::string 若存在返回如 ".tiff"、".tmp"；若不存在返回空串。
 *
 * @examples "aaa.tiff.tmp" -> ".tmp"；"readme" -> ""；".bashrc" -> ".bashrc"。
 */
GLOBALBASE_PORT std::string GB_GetFileExt(const std::string& filePathUtf8);

/**
 * @brief 获取父目录路径（统一使用正斜杠，且以“/”结尾）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8）。
 * @return std::string 父目录路径；若无分隔符则返回空串。
 *
 * @remarks 函数只做字符串层面的截取，不检查目录是否真实存在。
 */
GLOBALBASE_PORT std::string GB_GetDirectoryPath(const std::string& filePathUtf8);

/**
 * @brief 获取常规文件大小（字节）。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return size_t 成功返回字节数；失败返回 0。若底层文件大于 size_t 可表示的范围，返回 size_t 的最大值（截断）。
 *
 * @details Windows 通过 GetFileSizeEx 获取 64 位大小；Linux 使用 stat 的 st_size（仅对常规文件）。
 *          本函数不打开数据流进行读取，具备 O(1) 特性。
 */
GLOBALBASE_PORT size_t GB_GetFileSizeByte(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeKB(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeMB(const std::string& filePathUtf8);
GLOBALBASE_PORT double GB_GetFileSizeGB(const std::string& filePathUtf8);

/**
 * @brief 获取当前可执行程序所在目录（UTF-8）。
 *
 * @return std::string  返回目录（UTF-8）。失败返回空串。
 *
 * @remarks
 *  - Windows：通过 GetModuleFileNameW(nullptr, ...) 获取当前模块完整路径，再去掉文件名。该函数用于本进程更高效可靠。
 *  - Linux：通过 readlink("/proc/self/exe", ...) 读取指向当前进程可执行文件的内核符号链接；readlink 不会写入 NUL，需要手动补齐。
 *  - 两端统一将反斜杠替换为'/'，并规范化结尾只有一个'/'。
 */
GLOBALBASE_PORT std::string GB_GetExeDirectory();

/**
 * @brief 递归创建空文件：确保父目录存在后，在目标路径创建 0 字节文件。
 *
 * @param filePathUtf8        目标文件完整路径（UTF-8，分隔符可混用；内部统一处理）。
 * @param overwriteIfExists   是否在已存在时覆盖（截断到 0 字节）。
 *                            - true（默认）：若文件已存在则**截断为 0**；不存在则创建。
 *                            - false：仅当文件**不存在**时创建（原子语义）。若已存在且大小为 0 返回 true；
 *                                      若已存在但大小 > 0 或不是常规文件则返回 false。
 *
 * @return true  成功（新建或满足条件时保持/截断为 0 字节）；
 * @return false 失败（父目录无法创建、路径指向目录、权限不足、底层系统调用失败等）。
 *
 */
GLOBALBASE_PORT bool GB_CreateFileRecursive(const std::string& filePathUtf8, bool overwriteIfExists = true);

/**
 * @brief 计算路径 A 相对于路径 B 的相对路径（UTF-8）。
 *
 * @param pathAUtf8 目标路径 A（文件或目录，UTF-8）。Windows 下允许混用“/”与“\\”作为分隔符。
 * @param pathBUtf8 基准路径 B（文件或目录，UTF-8）。Windows 下允许混用“/”与“\\”作为分隔符。
 * @return std::string 相对路径（UTF-8），统一使用“/”作为分隔符。
 *
 * @details
 *  - 若 pathBUtf8 为空，则以 "." 作为基准目录。
 *  - 若 pathBUtf8 指向文件（或被视作文件），则以其父目录作为基准目录（等价于“相对于该文件所在目录”）。
 *  - 相对路径计算为 lexical（字符串）层面：会标准化分隔符、消解多余的 "." 与 ".." 片段，但不会解析符号链接。
 *  - 若 A 与 B 处于不同根（Windows 不同盘符 / 不同 UNC share），无法构造相对路径时，返回规范化后的 A（仍使用“/”）。
 *  - 若 A 被判定为目录（路径末尾带分隔符，或在文件系统中存在且为目录），返回结果末尾保证带“/”。
 *
 * @remarks
 *  - 为判定 A/B 是否目录，函数可能会访问文件系统（GB_IsDirectoryExists）。若希望完全避免访问文件系统，
 *    请在目录路径末尾显式追加分隔符（如 "a/b/" 或 "a\\b\\"）。
 */
GLOBALBASE_PORT std::string GB_GetRelativePath(const std::string& pathAUtf8, const std::string& pathBUtf8);

/**
 * @brief 拼接两个路径并进行 lexical 规范化（UTF-8）。
 *
 * @param leftPathUtf8  左侧路径（UTF-8）。Windows 下允许混用“/”与“\\”作为分隔符。
 * @param rightPathUtf8 右侧路径（UTF-8）。Windows 下允许混用“/”与“\\”作为分隔符。
 * @return std::string  拼接并规范化后的路径（UTF-8），统一使用“/”作为分隔符。
 *
 * @details
 *  - 若 rightPathUtf8 为空：返回 leftPathUtf8 的规范化结果。
 *  - 若 rightPathUtf8 为绝对路径：忽略 leftPathUtf8，直接返回 rightPathUtf8 的规范化结果。
 *  - 若 leftPathUtf8 存在且为文件，则将其父目录作为拼接基准（等价于“相对于该文件所在目录进行拼接”）。
 *  - 规范化会消解多余的 "." 与 ".." 片段（lexical），但不会解析符号链接。
 *  - Windows 输入允许“/”与“\\”，输出统一使用“/”。
 *  - 若输出路径表示目录（例如 rightPathUtf8 以分隔符结尾、或最后一个片段为 "."/".."，
 *    或输出路径在文件系统中存在且为目录），则返回结果末尾保证带“/”。
 *
 * @remarks
 *  - 为了更准确地区分“文件/目录”，函数可能会访问文件系统做存在性与类型判断（stat / GetFileAttributesW）。
 *    若希望完全避免访问文件系统，请在“目录路径”末尾显式追加分隔符（如 "a/b/" 或 "a\\b\\"）。
 */
GLOBALBASE_PORT std::string GB_JoinPath(const std::string& leftPathUtf8, const std::string& rightPathUtf8);


#endif