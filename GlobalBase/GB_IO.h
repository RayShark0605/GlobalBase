#ifndef GLOBALBASE_IO_H_H
#define GLOBALBASE_IO_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include <cstddef>
#include <cstdint>
#include <string>

class GB_Variant;

/**
 * @brief 将 UTF-8 文本写入文件。
 *
 * @param filePathUtf8    目标文件路径（UTF-8）。
 * @param utf8Content     待写入的 UTF-8 文本内容。
 * @param appendMode      是否以追加模式写入。
 *                        - false：覆盖写入；
 *                        - true：若文件存在则追加到末尾，否则新建文件。
 * @param addBomIfNewFile 当目标文件原本不存在时，是否在文件开头写入 UTF-8 BOM。
 *
 * @return true  写入成功；
 * @return false 写入失败（例如路径非法、父目录创建失败、文件无法打开或底层写入失败）。
 *
 * @remarks
 *  - 路径按 UTF-8 处理；Windows 内部会转换为 UTF-16 后调用宽字符 API。
 *  - 仅在“新建文件”且 addBomIfNewFile=true 时写入 BOM；对已有文件的追加写入不会重复写入 BOM。
 *  - 实现采用分块写入，适合较大文本文件场景。
 */
GLOBALBASE_PORT bool GB_WriteUtf8ToFile(const std::string& filePathUtf8, const std::string& utf8Content, bool appendMode = false, bool addBomIfNewFile = true);

/**
 * @brief 以原始字节方式读取整个文件，并直接构造成 std::string 返回。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return std::string 文件的原始字节内容；读取失败或文件为空时返回空字符串。
 *
 * @remarks
 *  - 本函数不做任何编码识别、BOM 去除或转码处理。
 *  - 返回值中的每个 char 仅表示原始字节，不保证是可直接显示的文本。
 *  - 若文件内容过大，超出 std::string 可表示范围，则返回空字符串。
 */
GLOBALBASE_PORT std::string GB_ReadFromFile(const std::string& filePathUtf8);

/**
 * @brief 读取文本文件并转换为 UTF-8 字符串。
 *
 * @param filePathUtf8      文件路径（UTF-8）。
 * @param fileEncodingName  文件原始编码名称，例如 "utf-8"、"gbk"、"gb18030" 等。
 *
 * @return std::string 转换后的 UTF-8 文本；读取失败、文件为空时返回空字符串。
 *
 * @remarks
 *  - 本函数先按原始字节读取文件，再根据 fileEncodingName 执行转码。
 *  - 若转码过程中抛出异常，函数会退化为直接返回原始字节串，以尽量保留原始内容。
 *  - 调用者应保证传入的编码名称与文件实际编码一致，否则结果可能出现乱码。
 */
GLOBALBASE_PORT std::string GB_ReadUtf8FromFile(const std::string& filePathUtf8, const std::string& fileEncodingName = "utf-8");

/**
 * @brief 将二进制字节缓冲区完整写入文件。
 *
 * @param data         待写入的二进制数据。
 * @param filePathUtf8 目标文件路径（UTF-8）。
 * @return true  写入成功；
 * @return false 写入失败。
 *
 * @remarks
 *  - 写入前会自动确保父目录存在。
 *  - 采用覆盖写入语义；若文件已存在，将被截断后重写。
 *  - 适合写入图片、序列化结果、压缩包、网络报文等任意二进制内容。
 */
GLOBALBASE_PORT bool GB_WriteBinaryToFile(const GB_ByteBuffer& data, const std::string& filePathUtf8);

/**
 * @brief 将 std::string 中保存的原始字节完整写入文件。
 *
 * @param data         待写入的原始字节串。
 * @param filePathUtf8 目标文件路径（UTF-8）。
 * @return true  写入成功；
 * @return false 写入失败。
 *
 * @remarks
 *  - 该重载将 std::string 视为“字节容器”而非文本容器，不做任何编码处理。
 *  - 实现直接使用 string 的底层内存写入文件，避免额外的数据拷贝。
 */
GLOBALBASE_PORT bool GB_WriteBinaryToFile(const std::string& data, const std::string& filePathUtf8);

/**
 * @brief 以二进制方式读取整个文件。
 *
 * @param filePathUtf8 文件路径（UTF-8）。
 * @return GB_ByteBuffer 文件全部字节内容；读取失败或文件为空时返回空缓冲区。
 *
 * @remarks
 *  - Windows 与 Linux 均采用底层文件 API 分块读取，适合较大的二进制文件。
 *  - 若文件在读取过程中被外部截断、替换或读取失败，函数会返回空缓冲区。
 *  - 本函数仅面向“完整读入内存”的场景，不适合超大文件的流式处理。
 */
GLOBALBASE_PORT GB_ByteBuffer GB_ReadBinaryFromFile(const std::string& filePathUtf8);

/**
 * @brief 面向 GB_ByteBuffer 的基础二进制读写辅助工具。
 *
 * @details
 *  当前提供了若干固定宽度标量类型在“小端序”下的追加与读取接口，便于实现轻量级二进制序列化。
 *  写入接口会将指定值按 little-endian 字节顺序追加到缓冲区末尾；
 *  读取接口会从 offset 指定位置开始解析，成功后自动推进 offset。
 */
class GLOBALBASE_PORT GB_ByteBufferIO
{
public:
    /**
     * @brief 将一个 uint16_t 以小端序追加到缓冲区末尾。
     *
     * @param buffer 目标缓冲区。
     * @param value  待追加的 16 位无符号整数。
     */
    static void AppendUInt16LE(GB_ByteBuffer& buffer, uint16_t value);

    /**
     * @brief 将一个 uint32_t 以小端序追加到缓冲区末尾。
     *
     * @param buffer 目标缓冲区。
     * @param value  待追加的 32 位无符号整数。
     */
    static void AppendUInt32LE(GB_ByteBuffer& buffer, uint32_t value);

    /**
     * @brief 将一个 uint64_t 以小端序追加到缓冲区末尾。
     *
     * @param buffer 目标缓冲区。
     * @param value  待追加的 64 位无符号整数。
     */
    static void AppendUInt64LE(GB_ByteBuffer& buffer, uint64_t value);

    /**
     * @brief 将一个 double 的 IEEE 754 原始比特位以小端序追加到缓冲区末尾。
     *
     * @param buffer 目标缓冲区。
     * @param value  待追加的 double 值。
     *
     * @remarks
     *  本函数按内存比特位序列化 double，不做文本格式化，也不做精度裁剪。
     */
    static void AppendDoubleLE(GB_ByteBuffer& buffer, double value);

    /**
     * @brief 从缓冲区的当前偏移位置读取一个小端序 uint16_t。
     *
     * @param buffer 源缓冲区。
     * @param offset 输入输出参数，表示当前读取偏移；读取成功后自动加 2。
     * @param value  输出读取到的 16 位无符号整数。
     * @return true  读取成功；
     * @return false 缓冲区剩余字节不足 2。
     */
    static bool ReadUInt16LE(const GB_ByteBuffer& buffer, size_t& offset, uint16_t& value);

    /**
     * @brief 从缓冲区的当前偏移位置读取一个小端序 uint32_t。
     *
     * @param buffer 源缓冲区。
     * @param offset 输入输出参数，表示当前读取偏移；读取成功后自动加 4。
     * @param value  输出读取到的 32 位无符号整数。
     * @return true  读取成功；
     * @return false 缓冲区剩余字节不足 4。
     */
    static bool ReadUInt32LE(const GB_ByteBuffer& buffer, size_t& offset, uint32_t& value);

    /**
     * @brief 从缓冲区的当前偏移位置读取一个小端序 uint64_t。
     *
     * @param buffer 源缓冲区。
     * @param offset 输入输出参数，表示当前读取偏移；读取成功后自动加 8。
     * @param value  输出读取到的 64 位无符号整数。
     * @return true  读取成功；
     * @return false 缓冲区剩余字节不足 8。
     */
    static bool ReadUInt64LE(const GB_ByteBuffer& buffer, size_t& offset, uint64_t& value);

    /**
     * @brief 从缓冲区的当前偏移位置读取一个按小端序存储的 double。
     *
     * @param buffer 源缓冲区。
     * @param offset 输入输出参数，表示当前读取偏移；读取成功后自动加 8。
     * @param value  输出读取到的 double 值。
     * @return true  读取成功；
     * @return false 缓冲区剩余字节不足 8，或底层 uint64_t 读取失败。
     *
     * @remarks
     *  本函数按 IEEE 754 原始比特位恢复 double，不做额外数值校验。
     */
    static bool ReadDoubleLE(const GB_ByteBuffer& buffer, size_t& offset, double& value);
};


/**
 * @brief 流定位基准。
 */
enum class GB_StreamSeekOrigin
{
    Begin = 0,
    Current,
    End
};

/**
 * @brief 流访问权限。
 */
enum class GB_StreamAccessMode
{
    ReadOnly = 0,
    WriteOnly,
    ReadWrite
};

/**
 * @brief 文件打开方式。
 */
enum class GB_FileStreamOpenMode
{
    // 仅打开已经存在的文件；不存在则打开失败。
    OpenExisting = 0,

    // 新建文件；文件已存在则打开失败。
    CreateNew,

    // 新建或覆盖文件；文件已存在时直接截断为 0 字节。
    CreateAlways,

    // 打开已有文件；不存在时新建文件。
    OpenAlways,

    // 打开已有文件并截断为 0 字节；文件不存在则打开失败。
    TruncateExisting
};

/**
 * @brief Windows 文件共享模式。
 *
 * @remarks
 *  - Windows 下会映射为 CreateFileW 的 FILE_SHARE_READ / FILE_SHARE_WRITE / FILE_SHARE_DELETE。
 *  - Linux/POSIX 常规 open() 不提供强制共享模式，本选项仅保留语义，不做强制限制。
 */
enum class GB_FileShareMode
{
    None = 0,
    Read = 1,
    Write = 2,
    Delete = 4,
    ReadWrite = 3,
    All = 7
};

inline GB_FileShareMode operator|(GB_FileShareMode leftMode, GB_FileShareMode rightMode)
{
    return static_cast<GB_FileShareMode>(static_cast<unsigned int>(leftMode) | static_cast<unsigned int>(rightMode));
}

inline GB_FileShareMode operator&(GB_FileShareMode leftMode, GB_FileShareMode rightMode)
{
    return static_cast<GB_FileShareMode>(static_cast<unsigned int>(leftMode) & static_cast<unsigned int>(rightMode));
}

/**
 * @brief 文件流打开选项。
 */
struct GLOBALBASE_PORT GB_FileStreamOpenOptions
{
    // 访问权限。
    GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadOnly;

    // 打开方式。
    GB_FileStreamOpenMode openMode = GB_FileStreamOpenMode::OpenExisting;

    // 共享模式。默认允许其它进程读、写和删除/重命名，以减少工具库对外部流程的干扰。
    GB_FileShareMode shareMode = GB_FileShareMode::All;

    // 打开前是否自动创建父目录。仅当 openMode 可能创建或写入文件时建议开启。
    bool createParentDirectories = true;

    // 打开后是否把当前位置移动到文件末尾，适合追加写入场景。
    bool seekToEndAfterOpen = false;

    // 是否向操作系统声明“顺序访问”倾向。适合大文件线性读写。
    bool sequentialAccessHint = false;

    // 是否尽量绕过延迟写回。该选项会显著降低写入性能，仅用于强持久化要求场景。
    bool writeThrough = false;
};

/**
 * @brief 二进制流抽象接口。
 *
 * @details
 *  该接口统一文件流与内存流的常用操作，包括 tell、length、seek、truncate、readBytes、writeBytes、flush、close 等。
 *  约定：
 *  - ReadBytes() 允许读取到 EOF，因此返回 true 只表示操作本身未失败，实际读取字节数由 outReadBytes 返回；
 *  - ReadExactBytes() 才表示必须完整读取指定字节数；
 *  - WriteBytes() 按“全量写入”语义实现，只要少写或失败即返回 false；
 *  - Tell()/Length() 的无参重载在失败时返回 UINT64_MAX，严谨场景应优先使用带输出参数的重载。
 */
class GLOBALBASE_PORT GB_Stream
{
public:
    virtual ~GB_Stream();

    /**
     * @brief 判断流当前是否处于打开状态。
     *
     * @return true  流可继续进行其支持的读写/定位操作；
     * @return false 流未打开、已经关闭或对象已被移动。
     */
    virtual bool IsOpen() const = 0;

    /**
     * @brief 判断当前流是否允许读取。
     *
     * @return true  当前打开状态和访问权限允许读取；
     * @return false 当前流未打开或以只写方式打开。
     */
    virtual bool CanRead() const = 0;

    /**
     * @brief 判断当前流是否允许写入。
     *
     * @return true  当前打开状态和访问权限允许写入；
     * @return false 当前流未打开或以只读方式打开。
     */
    virtual bool CanWrite() const = 0;

    /**
     * @brief 判断当前流是否支持随机定位。
     *
     * @return true  支持 Tell()/Seek()/Length()/Truncate() 这类定位相关操作；
     * @return false 不支持随机定位。
     */
    virtual bool IsSeekable() const = 0;

    /**
     * @brief 获取当前读写位置。
     *
     * @param outPosition 输出当前位置，单位为字节，从流起始位置 0 开始计数。
     * @return true  获取成功；
     * @return false 流未打开、不支持定位或底层系统调用失败。
     *
     * @remarks
     *  对文件流而言，该位置对应操作系统文件指针；对内存流而言，对应内部缓冲区偏移。
     */
    virtual bool Tell(std::uint64_t& outPosition) const = 0;

    /**
     * @brief 获取当前流长度。
     *
     * @param outLength 输出流长度，单位为字节。
     * @return true  获取成功；
     * @return false 流未打开、不支持长度查询或底层系统调用失败。
     *
     * @remarks
     *  本函数不会改变当前读写位置。
     */
    virtual bool Length(std::uint64_t& outLength) const = 0;

    /**
     * @brief 移动当前读写位置。
     *
     * @param offset 相对于 origin 的有符号偏移量，单位为字节。
     * @param origin 定位基准，可选流起始位置、当前位置或流末尾。
     * @return true  定位成功；
     * @return false 流未打开、不支持定位、偏移溢出或目标位置非法。
     *
     * @remarks
     *  内存流允许定位到当前长度之后；随后写入时，中间空洞会由 vector::resize() 填 0。
     */
    virtual bool Seek(std::int64_t offset, GB_StreamSeekOrigin origin = GB_StreamSeekOrigin::Begin) = 0;

    /**
     * @brief 调整流长度。
     *
     * @param length 新长度，单位为字节。
     * @return true  调整成功；
     * @return false 流未打开、不可写、长度超出平台限制或底层系统调用失败。
     *
     * @remarks
     *  缩短流会丢弃尾部数据；扩展流时新增区域的内容由具体实现决定，内存流为 0，文件流遵循操作系统语义。
     */
    virtual bool Truncate(std::uint64_t length) = 0;

    /**
     * @brief 刷新流。
     *
     * @return true  刷新成功或当前实现无须额外刷新；
     * @return false 流未打开或底层刷新失败。
     *
     * @remarks
     *  文件流当前实现会尝试将数据刷新到系统文件缓冲；内存流无额外写回动作，仅检查打开状态。
     */
    virtual bool Flush() = 0;

    /**
     * @brief 关闭流并释放底层资源。
     *
     * @remarks
     *  Close() 可重复调用；关闭后 IsOpen() 返回 false，后续读写/定位操作失败。
     */
    virtual void Close() = 0;

    /**
     * @brief 从当前位置读取最多 byteSize 个字节。
     *
     * @param outData 输出缓冲区，byteSize 大于 0 时不能为空。
     * @param byteSize 期望读取的最大字节数。
     * @param outReadBytes 实际读取字节数。读到 EOF 时可能小于 byteSize，甚至为 0。
     * @return true  读取过程未发生底层错误；
     * @return false 参数非法、不可读或底层读取失败。
     */
    virtual bool ReadBytes(void* outData, std::size_t byteSize, std::size_t& outReadBytes) = 0;

    /**
     * @brief 从当前位置写入 byteSize 个字节。
     *
     * @param data 输入缓冲区，byteSize 大于 0 时不能为空。
     * @param byteSize 需要写入的字节数。
     * @return true  指定字节全部写入成功；
     * @return false 参数非法、不可写或底层写入失败/少写。
     */
    virtual bool WriteBytes(const void* data, std::size_t byteSize) = 0;

    /**
     * @brief 获取当前读写位置；失败时返回 UINT64_MAX。
     *
     * @remarks
     *  严谨场景建议优先使用 Tell(std::uint64_t&) 以区分真实位置和失败状态。
     */
    std::uint64_t Tell() const;

    /**
     * @brief 获取当前流长度；失败时返回 UINT64_MAX。
     *
     * @remarks
     *  严谨场景建议优先使用 Length(std::uint64_t&) 以区分真实长度和失败状态。
     */
    std::uint64_t Length() const;

    /**
     * @brief 判断当前位置是否已经到达或超过流末尾。
     *
     * @return true  已到达末尾，或 Tell()/Length() 查询失败；
     * @return false 当前仍位于流长度范围内。
     */
    bool IsEnd() const;

    /**
     * @brief 精确读取 byteSize 个字节。
     *
     * @return true  指定字节全部读取成功；
     * @return false 读到 EOF、底层读取失败或参数非法。
     */
    bool ReadExactBytes(void* outData, std::size_t byteSize);

    /**
     * @brief 从当前位置读取最多 byteSize 个字节到 GB_ByteBuffer。
     *
     * @param byteSize 期望读取的最大字节数。
     * @param outData 输出缓冲区，函数开始时会被清空。
     * @return true  读取过程未发生底层错误；
     * @return false 分配失败或底层读取失败。
     */
    bool ReadBytes(std::size_t byteSize, GB_ByteBuffer& outData);

    /**
     * @brief 从当前位置读取到流末尾。
     *
     * @param outData 输出缓冲区，函数开始时会被清空。
     * @param maxBytes 最大允许读取字节数，0 表示不限制。
     * @return true  读取成功；
     * @return false 长度查询失败、超过 maxBytes、分配失败或读取失败。
     */
    bool ReadToEnd(GB_ByteBuffer& outData, std::uint64_t maxBytes = 0);

    /**
     * @brief 写入整个 GB_ByteBuffer。
     */
    bool WriteBytes(const GB_ByteBuffer& data);

    /**
     * @brief 将 std::string 作为原始字节块写入，不做编码转换。
     */
    bool WriteBytes(const std::string& data);

    /**
     * @brief 写入带长度前缀的二进制缓冲区。
     *
     * @remarks
     *  长度前缀使用 7-bit VarUInt 编码，小数据通常只占 1 个字节，避免固定 8 字节长度造成空间浪费。
     */
    bool WriteByteBuffer(const GB_ByteBuffer& data);

    /**
     * @brief 读取带长度前缀的二进制缓冲区。
     *
     * @param outData 输出缓冲区，函数开始时会被清空。
     * @param maxBytes 最大允许读取字节数，0 表示不限制。
     */
    bool ReadByteBuffer(GB_ByteBuffer& outData, std::uint64_t maxBytes = 0);

    /**
     * @brief 写入带长度前缀的字符串原始字节。
     *
     * @remarks
     *  本函数不做编码转换；长度前缀使用 7-bit VarUInt 编码。
     */
    bool WriteString(const std::string& text);

    /**
     * @brief 读取带长度前缀的字符串原始字节。
     *
     * @param outText 输出字符串，函数开始时会被清空。
     * @param maxBytes 最大允许读取字节数，0 表示不限制。
     */
    bool ReadString(std::string& outText, std::uint64_t maxBytes = 0);

    /**
     * @brief 将 GB_Variant 完整序列化后写入流。
     *
     * @param value 待写入的 Variant 对象。
     * @return true  写入成功；
     * @return false 序列化失败或底层流写入失败。
     *
     * @remarks
     *  - 内建类型使用紧凑专用格式写入，避免 GB_Variant 完整序列化头部带来的额外空间开销。
     *  - 对未注册或无法通过公开接口紧凑表达的自定义类型，自动退回 GB_Variant::Serialize() 完整格式。
     *  - ReadVariant() 会按对应格式恢复 Variant，内建类型会尽量保留原始 C++ 精确类型。
     */
    bool WriteVariant(const GB_Variant& value);

    /**
     * @brief 从流中读取并反序列化一个 GB_Variant。
     *
     * @param outValue 输出 Variant。成功时写入反序列化结果；失败时保持原值不变。
     * @param maxBytes 允许读取的最大序列化字节数，0 表示不限制。
     * @return true  读取并反序列化成功；
     * @return false 底层读取失败、数据超过限制或 Variant 反序列化失败。
     *
     * @remarks
     *  - 本函数读取的格式必须与 WriteVariant() 保持一致。
     *  - maxBytes 建议在读取不可信输入时设置，用于限制字符串、二进制或完整序列化数据的内存分配规模。
     */
    bool ReadVariant(GB_Variant& outValue, std::uint64_t maxBytes = 0);
};

/**
 * @brief 文件流。
 *
 * @details
 *  Windows 下直接使用 CreateFileW/ReadFile/WriteFile 等 API，以保证 UTF-8 路径、64 位文件位置、共享模式和截断语义可控。
 *  Linux/POSIX 下使用 open/read/write/lseek/ftruncate。类本身遵循 RAII，析构时自动关闭文件句柄。
 */
class GLOBALBASE_PORT GB_FileStream : public GB_Stream
{
public:
    GB_FileStream();
    explicit GB_FileStream(const std::string& filePathUtf8, const GB_FileStreamOpenOptions& options = GB_FileStreamOpenOptions());
    ~GB_FileStream() override;

    GB_FileStream(const GB_FileStream& other) = delete;
    GB_FileStream& operator=(const GB_FileStream& other) = delete;

    GB_FileStream(GB_FileStream&& other) noexcept;
    GB_FileStream& operator=(GB_FileStream&& other) noexcept;

    using GB_Stream::ReadBytes;
    using GB_Stream::WriteBytes;

    bool Open(const std::string& filePathUtf8, const GB_FileStreamOpenOptions& options = GB_FileStreamOpenOptions());
    std::string GetFilePathUtf8() const;

    bool IsOpen() const override;
    bool CanRead() const override;
    bool CanWrite() const override;
    bool IsSeekable() const override;

    bool Tell(std::uint64_t& outPosition) const override;
    bool Length(std::uint64_t& outLength) const override;
    bool Seek(std::int64_t offset, GB_StreamSeekOrigin origin = GB_StreamSeekOrigin::Begin) override;
    bool Truncate(std::uint64_t length) override;
    bool Flush() override;
    void Close() override;

    bool ReadBytes(void* outData, std::size_t byteSize, std::size_t& outReadBytes) override;
    bool WriteBytes(const void* data, std::size_t byteSize) override;

private:
    struct Impl;
    Impl* impl_;
};

/**
 * @brief 内存流。
 *
 * @details
 *  内存流以 GB_ByteBuffer 作为底层存储，支持随机读写、自动扩容、截断和移动取出缓冲区。
 *  对于只读内存流，所有写入、截断操作都会失败。
 */
class GLOBALBASE_PORT GB_MemoryStream : public GB_Stream
{
public:
    /**
     * @brief 构造一个空的可读写内存流。
     *
     * @remarks
     *  - 底层缓冲区由流对象内部持有。
     *  - 写入数据可通过 GetBuffer() 查看，也可通过 TakeBuffer() 移出。
     */
    GB_MemoryStream();

    /**
     * @brief 构造一个空内存流，并指定访问权限。
     *
     * @param accessMode 访问权限。
     */
    explicit GB_MemoryStream(GB_StreamAccessMode accessMode);

    /**
     * @brief 以拷贝方式构造内存流。
     *
     * @param data       初始缓冲区。
     * @param accessMode 访问权限。
     *
     * @remarks
     *  本构造函数会复制 @p data，后续读写只影响流对象内部副本，不会修改传入对象。
     */
    explicit GB_MemoryStream(const GB_ByteBuffer& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 绑定外部缓冲区构造内存流。
     *
     * @param data       外部缓冲区。该对象的生命周期必须长于本内存流的打开期间。
     * @param accessMode 访问权限。
     *
     * @remarks
     *  - 本构造函数不会复制 @p data，而是直接以 @p data 作为底层存储。
     *  - 对流的写入、截断、清空等操作会立即反映到外部缓冲区。
     *  - Flush() 对内存流没有额外写回动作，只用于保持与 GB_Stream 接口一致。
     *  - 若希望强制拷贝非 const 变量，可显式传入 const GB_ByteBuffer& 或临时副本。
     */
    explicit GB_MemoryStream(GB_ByteBuffer& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 以移动方式构造内存流。
     *
     * @param data       初始缓冲区。其内容会被移动到流对象内部。
     * @param accessMode 访问权限。
     */
    explicit GB_MemoryStream(GB_ByteBuffer&& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    ~GB_MemoryStream() override;

    GB_MemoryStream(const GB_MemoryStream& other) = delete;
    GB_MemoryStream& operator=(const GB_MemoryStream& other) = delete;

    GB_MemoryStream(GB_MemoryStream&& other) noexcept;
    GB_MemoryStream& operator=(GB_MemoryStream&& other) noexcept;

    using GB_Stream::ReadBytes;
    using GB_Stream::WriteBytes;

    /**
     * @brief 重新打开为空内存流。
     *
     * @param accessMode 访问权限。
     *
     * @remarks
     *  本函数会解除外部缓冲区绑定，清空内部缓冲区，并把当前位置重置为 0。
     */
    void Open(GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 以拷贝方式重置内存流内容。
     *
     * @param data       新的初始缓冲区。
     * @param accessMode 访问权限。
     *
     * @remarks
     *  本函数会解除外部缓冲区绑定，并复制 @p data 到流对象内部。
     */
    void Reset(const GB_ByteBuffer& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 绑定外部缓冲区并重置内存流。
     *
     * @param data       外部缓冲区。该对象的生命周期必须长于本内存流的打开期间。
     * @param accessMode 访问权限。
     *
     * @remarks
     *  后续写入、截断、清空等操作会直接修改 @p data。
     */
    void Reset(GB_ByteBuffer& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 以移动方式重置内存流内容。
     *
     * @param data       新的初始缓冲区。其内容会被移动到流对象内部。
     * @param accessMode 访问权限。
     *
     * @remarks
     *  本函数会解除外部缓冲区绑定。
     */
    void Reset(GB_ByteBuffer&& data, GB_StreamAccessMode accessMode = GB_StreamAccessMode::ReadWrite);

    /**
     * @brief 获取当前底层缓冲区。
     *
     * @return const GB_ByteBuffer& 当前缓冲区；若对象已被移动导致无实现体，则返回静态空缓冲区。
     *
     * @remarks
     *  若当前绑定外部缓冲区，则返回的正是该外部缓冲区。
     */
    const GB_ByteBuffer& GetBuffer() const;

    /**
     * @brief 取出当前缓冲区内容。
     *
     * @return GB_ByteBuffer 当前缓冲区内容。
     *
     * @remarks
     *  - 内部持有模式下，本函数会移动内部缓冲区并清空流内容。
     *  - 外部绑定模式下，本函数会移动外部缓冲区内容，外部缓冲区随后变为空。
     */
    GB_ByteBuffer TakeBuffer();

    /**
     * @brief 清空当前缓冲区并将当前位置重置为 0。
     *
     * @return true  清空成功；
     * @return false 当前流不可写。
     */
    bool ClearBuffer();

    /**
     * @brief 预留底层缓冲区容量。
     *
     * @param capacity 期望容量。
     * @return true  预留成功；
     * @return false 当前流不可写、容量超出 size_t 范围或内存分配失败。
     *
     * @remarks
     *  当调用者能够预估即将写入的数据量时，提前 Reserve() 可减少 vector 扩容次数。
     */
    bool Reserve(std::uint64_t capacity);

    /**
     * @brief 判断当前内存流是否正在使用外部缓冲区。
     */
    bool IsUsingExternalBuffer() const;

    bool IsOpen() const override;
    bool CanRead() const override;
    bool CanWrite() const override;
    bool IsSeekable() const override;

    bool Tell(std::uint64_t& outPosition) const override;
    bool Length(std::uint64_t& outLength) const override;
    bool Seek(std::int64_t offset, GB_StreamSeekOrigin origin = GB_StreamSeekOrigin::Begin) override;
    bool Truncate(std::uint64_t length) override;
    bool Flush() override;
    void Close() override;

    bool ReadBytes(void* outData, std::size_t byteSize, std::size_t& outReadBytes) override;
    bool WriteBytes(const void* data, std::size_t byteSize) override;

private:
    struct Impl;
    Impl* impl_;
};


#endif
