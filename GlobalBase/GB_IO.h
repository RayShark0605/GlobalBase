#ifndef GLOBALBASE_IO_H_H
#define GLOBALBASE_IO_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include <string>

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

#endif
