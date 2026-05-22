#ifndef GLOBALBASE_FILESYSTEM_H_H
#define GLOBALBASE_FILESYSTEM_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include "GB_DateTime.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 文件系统路径类型。
 */
enum class GB_FileType
{
    NotExists = 0,     ///< 路径不存在。
    RegularFile = 1,   ///< 常规文件。
    Directory = 2,     ///< 目录。
    SymbolicLink = 3,  ///< 符号链接本身（不跟随目标）。
    Other = 4          ///< 其它类型，例如设备、管道、套接字或非符号链接的重解析点等。
};

/**
 * @brief 文件/目录属性信息。
 *
 * @remarks
 * - 所有路径均按 UTF-8 输入；Windows 内部转换为 UTF-16 后访问。
 * - 日期时间属性使用 GB_DateTime，并按 UTC 时间点保存（Spec()==GB_DateTimeSpec::UtcTime）。
 * - createdTime 在 POSIX 平台通常不可可靠获取；不可获取时为 GB_DateTime::Invalid。
 * - SymbolicLink 表示链接本身，不递归跟随链接目标。
 */
struct GB_FileAttributes
{
    GB_FileType fileType = GB_FileType::NotExists;
    uint64_t fileSizeByte = 0; ///< 仅常规文件返回有效大小；目录、符号链接及其它类型固定为 0。
    bool isReadOnly = false;
    bool isHidden = false;
    bool isSystem = false;
    bool isArchive = false;
    bool isTemporary = false;
    GB_DateTime createdTime = GB_DateTime::Invalid;
    GB_DateTime lastAccessTime = GB_DateTime::Invalid;
    GB_DateTime lastWriteTime = GB_DateTime::Invalid;
};

/**
 * @brief 文件/目录属性修改选项。
 *
 * @remarks
 * - changeXXX 为 true 时才会修改对应属性；为 false 时保持原值。
 * - Windows 支持 readOnly/hidden/system/archive/temporary 以及 created/access/write 时间。
 * - POSIX 平台只支持 readOnly（基于写权限位）以及 lastAccessTime/lastWriteTime；
 *   若请求修改 hidden/system/archive/temporary/createdTime，将返回 false。
 */
struct GB_FileAttributeModifyOptions
{
    bool changeReadOnly = false;
    bool readOnly = false;

    bool changeHidden = false;
    bool hidden = false;

    bool changeSystem = false;
    bool system = false;

    bool changeArchive = false;
    bool archive = false;

    bool changeTemporary = false;
    bool temporary = false;

    bool changeCreatedTime = false;
    GB_DateTime createdTime = GB_DateTime::Invalid;

    bool changeLastAccessTime = false;
    GB_DateTime lastAccessTime = GB_DateTime::Invalid;

    bool changeLastWriteTime = false;
    GB_DateTime lastWriteTime = GB_DateTime::Invalid;
};

/**
 * @brief 磁盘/分区空间信息（字节）。
 *
 * @remarks
 * - availableSpaceByte：当前用户可用空间。
 * - freeSpaceByte：分区总空闲空间。
 * - totalSpaceByte：分区总空间。
 */
struct GB_DiskSpaceInfo
{
    uint64_t availableSpaceByte = 0;
    uint64_t freeSpaceByte = 0;
    uint64_t totalSpaceByte = 0;
};

/**
 * @brief 判断给定 UTF-8 路径是否存在且为“常规文件”（不把目录、管道、设备等特殊对象视为文件）。
 *
 * @param filePathUtf8 目标路径（UTF-8）。支持混合分隔符，内部自动处理。
 * @return true  文件存在且为常规文件；
 * @return false 不存在、不是常规文件，或底层检查失败（例如 Windows 上 UTF-8→UTF-16 转换失败）。
 *
 * @remarks Windows 使用 GetFileAttributesW/FindFirstFileW 区分重解析点；POSIX 使用 lstat 并判定 S_ISREG。
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
 * @brief 获取路径类型（不跟随符号链接目标）。
 *
 * @param pathUtf8 文件或目录路径（UTF-8）。
 * @return GB_FileType 路径不存在返回 NotExists；符号链接返回 SymbolicLink；其它按实际类型返回。
 */
GLOBALBASE_PORT GB_FileType GB_GetFileType(const std::string& pathUtf8);

/**
 * @brief 判断任意文件系统路径是否存在（不跟随符号链接目标）。
 *
 * @param pathUtf8 文件、目录、符号链接或其它文件系统对象路径（UTF-8）。
 * @return true  路径项本身存在；
 * @return false 路径项不存在或底层检查失败。
 *
 * @remarks 与 GB_IsFileExists/GB_IsDirectoryExists 不同，本函数只判断路径项本身是否存在，
 *          因此 dangling symbolic link 也会被视为存在。
 */
GLOBALBASE_PORT bool GB_IsPathExists(const std::string& pathUtf8);

/**
 * @brief 获取文件或目录属性。
 *
 * @param pathUtf8       文件或目录路径（UTF-8）。
 * @param outAttributes  输出属性信息；函数开始时会先重置为默认值。
 * @return true  获取成功且路径存在；
 * @return false 路径不存在或底层读取失败。
 */
GLOBALBASE_PORT bool GB_GetFileAttributes(const std::string& pathUtf8, GB_FileAttributes& outAttributes);

/**
 * @brief 修改文件或目录属性。
 *
 * @param pathUtf8       文件或目录路径（UTF-8）。
 * @param modifyOptions  属性修改选项，仅 changeXXX=true 的字段生效。
 * @return true  修改成功或没有需要修改的属性但路径存在；
 * @return false 路径不存在、属性不受当前平台支持、日期时间无效或底层修改失败。
 */
GLOBALBASE_PORT bool GB_SetFileAttributes(const std::string& pathUtf8, const GB_FileAttributeModifyOptions& modifyOptions);

/**
 * @brief 判断文件是否可读。必须实际按只读模式尝试打开文件。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return true  文件可按只读模式打开；
 * @return false 文件不存在、不是常规文件、被独占占用或权限不足等。
 */
GLOBALBASE_PORT bool GB_CanReadFile(const std::string& filePathUtf8);

/**
 * @brief 判断文件是否可写。必须实际按只写模式尝试打开文件，不创建、不截断、不写入内容。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return true  文件可按只写模式打开；
 * @return false 文件不存在、不是常规文件、被独占占用、只读或权限不足等。
 */
GLOBALBASE_PORT bool GB_CanWriteFile(const std::string& filePathUtf8);

/**
 * @brief 判断文件是否可读写。必须实际按读写模式尝试打开文件，不创建、不截断、不写入内容。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return true  文件可按读写模式打开；
 * @return false 文件不存在、不是常规文件、被独占占用、只读或权限不足等。
 */
GLOBALBASE_PORT bool GB_CanReadWriteFile(const std::string& filePathUtf8);

/**
 * @brief 重命名文件或目录（仅修改同一父目录下的名称）。
 *
 * @param pathUtf8          原文件/目录路径（UTF-8）。
 * @param newNameUtf8       新名称（UTF-8），只能是名称，不能包含 '/' 或 '\\'。
 * @param overwriteIfExists 若目标已存在，是否允许覆盖。覆盖目录受系统限制，通常只支持覆盖常规文件。
 * @return true  重命名成功；
 * @return false 原路径不存在、新名称非法、目标存在且不允许覆盖，或底层重命名失败。
 */
GLOBALBASE_PORT bool GB_RenamePath(const std::string& pathUtf8, const std::string& newNameUtf8, bool overwriteIfExists = false);

/**
 * @brief 移动文件或目录到目标路径。
 *
 * @param srcPathUtf8       源文件/目录路径（UTF-8）。
 * @param dstPathUtf8       目标文件/目录路径（UTF-8）。
 * @param overwriteIfExists 若目标已存在，是否允许覆盖。覆盖目录受系统限制，通常只支持覆盖常规文件。
 * @return true  移动成功；
 * @return false 源不存在、目标冲突、目标位于源目录内部、递归复制/删除失败或底层移动失败。
 *
 * @remarks
 * - 优先使用系统原生 rename/move，通常同分区移动目录为 O(1) 元数据操作。
 * - 当跨分区/跨文件系统导致原生移动失败时，会退化为“递归复制到目标路径，再删除源路径”。目录会递归复制其所有子目录和常规文件。
 * - 递归移动目录时不会跟随目录符号链接/重解析点进入目标目录，以避免循环遍历或误移动链接目标。
 * - 若 overwriteIfExists=true 且目标已存在，会尽量以临时路径完成复制后再替换目标，以降低部分复制失败造成的数据风险。
 */
GLOBALBASE_PORT bool GB_MovePath(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists = false);

/**
 * @brief 获取当前工作目录。
 *
 * @return std::string 当前工作目录（UTF-8）；失败返回空串。返回路径统一使用 '/'，且保证以 '/' 结尾。
 */
GLOBALBASE_PORT std::string GB_GetCurrentDirectory();

/**
 * @brief 获取 Windows 快捷方式（.lnk）的目标路径。
 *
 * @param shortcutPathUtf8 快捷方式路径（UTF-8）。
 * @return std::string 目标路径（UTF-8，分隔符统一为 '/'）；失败或非 Windows 平台返回空串。
 *
 * @remarks Windows 使用 IShellLinkW/IPersistFile 读取 .lnk；若快捷方式指向 Shell 命名空间对象而非文件系统路径，可能返回空串。
 */
GLOBALBASE_PORT std::string GB_GetShortcutTargetPath(const std::string& shortcutPathUtf8);

/**
 * @brief 获取路径所在磁盘/分区的空间信息。
 *
 * @param pathUtf8      任意文件或目录路径（UTF-8）；若为空，则使用当前工作目录。
 * @param outSpaceInfo  输出空间信息；函数开始时会先重置为 0。
 * @return true  获取成功；
 * @return false 路径非法、所在分区不可访问或底层调用失败。
 */
GLOBALBASE_PORT bool GB_GetDiskSpaceInfo(const std::string& pathUtf8, GB_DiskSpaceInfo& outSpaceInfo);

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
 *          Windows 下若遇到目录类型的重解析点（符号链接/联接点等），将仅删除该重解析点本身，
 *          不递归进入其目标目录，以避免误删目标目录或形成循环遍历。
 *
 * @note   为安全起见，若 dirPathUtf8 指向文件系统根目录（如 "/"、"C:/"、"//server/share/"）或 "."/".."，将直接返回 false。
 */
GLOBALBASE_PORT bool GB_DeleteDirectory(const std::string& dirPathUtf8);

/**
 * @brief 删除单个常规文件。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return true  删除成功；
 * @return false 文件不存在、不是常规文件或底层删除失败。
 *
 * @remarks Windows 使用 DeleteFileW；POSIX 使用 unlink。不递归删除目录，也不删除目录符号链接。
 */
GLOBALBASE_PORT bool GB_DeleteFile(const std::string& filePathUtf8);

/**
 * @brief 删除任意路径项。
 *
 * @param pathUtf8 文件、目录、符号链接或其它路径项（UTF-8）。
 * @return true  删除成功；
 * @return false 路径不存在、根目录保护触发或底层删除失败。
 *
 * @remarks
 * - 常规文件/文件符号链接使用文件删除语义。
 * - 真实目录递归删除其所有内容。
 * - 目录符号链接/联接点只删除链接本身，不递归进入目标目录。
 * - 为安全起见，不允许删除文件系统根目录，也不允许通过 "."/".." 删除当前目录或父目录。
 */
GLOBALBASE_PORT bool GB_DeletePath(const std::string& pathUtf8);

/**
 * @brief 复制单个常规文件到目标路径（允许覆盖）。
 *
 * @param srcFilePathUtf8 源文件（UTF-8）。
 * @param dstFilePathUtf8 目标文件（UTF-8），若已存在将被覆盖。
 * @return true  复制成功；
 * @return false 源不是常规文件、目标是目录、任一端打开失败、读写错误或系统调用失败。
 *
 * @notes Windows 使用 CopyFileExW（允许覆盖），会保留基础文件属性、扩展属性以及 NTFS 备用数据流等系统可复制元数据；
 *        POSIX 使用流式复制并尽量保留权限与访问/修改时间。
 *        不处理稀疏文件/洞洞文件、ACL、扩展属性、备用数据流等高级特性。
 */
GLOBALBASE_PORT bool GB_CopyFile(const std::string& srcFilePathUtf8, const std::string& dstFilePathUtf8);

/**
 * @brief 复制文件、目录或符号链接到目标路径。
 *
 * @param srcPathUtf8       源路径（UTF-8）。
 * @param dstPathUtf8       目标路径（UTF-8）。
 * @param overwriteIfExists 若目标已存在，是否允许替换。替换目录受系统能力与权限限制。
 * @return true  复制成功；
 * @return false 源不存在、目标冲突、目标位于源目录内部、递归复制失败或底层调用失败。
 *
 * @remarks
 * - 复制目录时不会跟随目录符号链接/重解析点深入目标，避免循环遍历。
 * - POSIX 平台尽量复制符号链接本身；Windows 平台默认不复制符号链接/其它重解析点。
 * - 顶层目标优先复制到同级临时路径后再改名到最终路径；覆盖已有目标时会先备份旧目标，尽量降低半成品暴露与目标提前截断风险。
 */
GLOBALBASE_PORT bool GB_CopyPath(const std::string& srcPathUtf8, const std::string& dstPathUtf8, bool overwriteIfExists = false);

/**
 * @brief 列出目录下所有“文件”的完整路径（不含目录），可选递归。
 *
 * @param dirPathUtf8 目录路径（UTF-8）。
 * @param recursive   是否递归子目录。
 * @return std::vector<std::string>  文件路径列表；若目录不存在或遍历失败返回空。
 *
 * @remarks 返回路径统一使用正斜杠“/”，且不以“/”结尾。遍历期间遇到不可读目录/条目会被跳过。
 *          为避免循环与跨树遍历，递归模式下不会跟随目录符号链接继续深入（Windows：目录 reparse point；Linux：符号链接）。
 */
GLOBALBASE_PORT std::vector<std::string> GB_GetFilesList(const std::string& dirPathUtf8, bool recursive = false);

/**
 * @brief 获取文件名（可选是否保留扩展名）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8），允许混合分隔符。
 * @param withExt      true 返回包含扩展名的文件名；false 返回去掉“最后一个点”之后扩展的文件名。
 * @return std::string 文件名（不含路径）。若最后一个路径段是 "." 或 ".." 则返回空串。
 *         特殊情况：以点开头且没有其它点的隐藏文件（如 ".bashrc"），当 withExt=false 时仍返回完整文件名 ".bashrc"。
 */
GLOBALBASE_PORT std::string GB_GetFileName(const std::string& filePathUtf8, bool withExt = false);

/**
 * @brief 获取“最后一个点”起算的扩展名（包含前导点）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8）。
 * @return std::string 若存在返回如 ".tiff"、".tmp"；若不存在返回空串。
 *
 * @examples "aaa.tiff.tmp" -> ".tmp"；"readme" -> ""；".bashrc" -> ""；".config.json" -> ".json"；"file." -> ""。
 */
GLOBALBASE_PORT std::string GB_GetFileExt(const std::string& filePathUtf8);

/**
 * @brief 获取父目录路径（统一使用正斜杠，且以“/”结尾）。
 *
 * @param filePathUtf8 完整路径或文件名（UTF-8）。
 * @return std::string 父目录路径；若无分隔符则返回空串。
 *
 * @remarks 函数只做字符串层面的截取，不检查目录是否真实存在。若输入为文件系统根目录（如 "/"、"C:/"、"//server/share"），返回空串。
 */
GLOBALBASE_PORT std::string GB_GetDirectoryPath(const std::string& filePathUtf8);

/**
 * @brief 获取常规文件大小（字节）。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return size_t 成功返回字节数；失败返回 0。若底层文件大于 size_t 可表示的范围，返回 size_t 的最大值（截断）。
 *
 * @details Windows 通过 GetFileAttributesExW 获取 64 位大小；Linux 使用 stat 的 st_size（仅对常规文件）。
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
 *  - Windows：通过 GetModuleFileNameW(nullptr, ...) 获取当前模块完整路径，再去掉文件名。
 *  - Linux：通过 readlink("/proc/self/exe", ...) 读取指向当前进程可执行文件的内核符号链接；readlink 不会写入 NUL，需要手动补齐。
 *  - 两端统一将反斜杠替换为'/'，并保证末尾只有一个'/'.
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
 */
GLOBALBASE_PORT std::string GB_JoinPath(const std::string& leftPathUtf8, const std::string& rightPathUtf8);

/**
 * @brief 获取临时文件目录。
 *
 * @remarks
 * - 返回 UTF-8 编码的 std::string。
 * - 路径分隔符统一使用 '/'。
 * - 返回的目录路径保证以 '/' 结尾。
 * - Windows：优先使用系统可用的 GetTempPath2W；若当前系统不可用，则回退到 GetTempPathW。
 * - Linux：优先使用 TMPDIR；否则使用 "/tmp/"。
 *
 * @return 临时目录路径；失败返回空字符串。
 */
GLOBALBASE_PORT std::string GB_GetTempDirectory();

/**
 * @brief 获取当前用户的桌面目录。
 *
 * @remarks
 * - 返回 UTF-8 编码的 std::string。
 * - 路径分隔符统一使用 '/'。
 * - 返回的目录路径保证以 '/' 结尾。
 * - Windows：使用 SHGetKnownFolderPath(FOLDERID_Desktop)。
 * - Linux：优先读取 XDG user-dirs（user-dirs.dirs 里的 XDG_DESKTOP_DIR）；否则回退到 "$HOME/Desktop/"。
 *
 * @return 桌面目录路径；失败返回空字符串。
 */
GLOBALBASE_PORT std::string GB_GetDesktopDirectory();

/**
 * @brief 获取当前用户主目录（Home/Profile）。
 *
 * @remarks
 * - 返回 UTF-8 编码的 std::string。
 * - 路径分隔符统一使用 '/'。
 * - 返回的目录路径保证以 '/' 结尾。
 * - Windows：使用 SHGetKnownFolderPath(FOLDERID_Profile)。
 * - Linux：优先使用 HOME；否则使用 getpwuid/getuid。
 *
 * @return 用户主目录路径；失败返回空字符串。
 */
GLOBALBASE_PORT std::string GB_GetHomeDirectory();

/**
 * @brief 获取当前用户“下载(Downloads)”目录。
 *
 * @remarks
 * - 返回 UTF-8 编码的 std::string。
 * - 路径分隔符统一使用 '/'。
 * - 返回的目录路径保证以 '/' 结尾。
 * - Windows：使用 SHGetKnownFolderPath(FOLDERID_Downloads)。
 * - Linux：优先读取 XDG user-dirs（user-dirs.dirs 里的 XDG_DOWNLOAD_DIR）；
 *   若系统未配置/不存在该文件，则回退到 "$HOME/Downloads/"。
 *
 * @return 下载目录路径；失败返回空字符串。
 */
GLOBALBASE_PORT std::string GB_GetDownloadsDirectory();

/**
 * @brief 根据文件二进制流（magic number / container 特征）尽可能猜测文件后缀名。
 *
 * @param fileBytes 文件二进制数据（GB_ByteBuffer，UTF-8 无关）。通常至少提供文件头若干 KB 会更可靠。
 * @return std::string 可能的后缀名（UTF-8，小写，包含前导点，例如 ".png"、".mp4"、".webp"）。
 *         若无法判断则返回空字符串。
 *
 * @remarks
 * - 该接口属于“尽力而为”的启发式判断：仅依赖文件内容特征，不依赖文件名。
 * - 对于容器格式（如 ISO-BMFF：mp4/mov/heic/avif 等），可能需要更多字节才能更准确地区分。
 */
GLOBALBASE_PORT std::string GB_GuessFileExt(const GB_ByteBuffer& fileBytes);

#endif