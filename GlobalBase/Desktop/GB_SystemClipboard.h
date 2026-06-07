#ifndef GLOBALBASE_SYSTEM_CLIPBOARD_H_H
#define GLOBALBASE_SYSTEM_CLIPBOARD_H_H

#include "GB_EventDispatcher.h"
#include "GB_SystemResult.h"
#include "../CV/GB_Image.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief 剪贴板文本换行处理策略。
 *
 * @remarks
 * - 该策略同时用于文本读取和文本写入。
 * - Preserve 适合严格保留来源文本；Lf 适合跨平台内部处理；CrLf 适合面向 Windows 文本控件、记事本、Office 等传统消费端。
 * - 换行规范化只处理 CR、LF、CRLF 三类常见换行序列，不修改其他 Unicode 空白字符。
 */
enum class GB_SystemClipboardNewlineMode : uint16_t
{
    /** @brief 保留输入中的 CR、LF、CRLF 原始形式，不做任何换行转换。 */
    Preserve = 0,

    /** @brief 将 CR、LF、CRLF 统一转换为 LF（"\n"）。 */
    Lf = 1,

    /** @brief 将 CR、LF、CRLF 统一转换为 CRLF（"\r\n"）。 */
    CrLf = 2
};

/**
 * @brief 打开系统剪贴板时的有限重试策略。
 *
 * @remarks
 * - Windows 剪贴板是当前 window station 内的全局共享资源，同一时刻通常只能被一个窗口或任务打开。
 * - 当其他进程短时间占用剪贴板时，本模块会按该策略进行有限重试，以降低偶发 ResourceBusy 的概率。
 * - 重试延迟从 initialRetryDelayMilliseconds 开始指数增长，并被 maxRetryDelayMilliseconds 限制。
 * - retryCount 表示首次尝试失败后允许追加的重试次数；总尝试次数最多为 1 + retryCount。
 * - maxTotalRetryDelayMilliseconds 用于限制所有重试 Sleep 的累计时长，避免异常配置导致调用线程长时间阻塞。
 * - 该结构只影响 OpenClipboard 阶段，不改变读取、转换、分配内存、SetClipboardData 等后续步骤的错误处理。
 */
struct GB_SystemClipboardAccessOptions
{
    /** @brief 首次打开失败后允许追加的重试次数；0 表示不重试。 */
    uint32_t retryCount = 8;

    /** @brief 第一次重试前的等待时间，单位为毫秒。 */
    uint32_t initialRetryDelayMilliseconds = 1;

    /** @brief 单次重试等待时间上限，单位为毫秒。 */
    uint32_t maxRetryDelayMilliseconds = 16;

    /** @brief 所有重试等待时间的累计上限，单位为毫秒。 */
    uint32_t maxTotalRetryDelayMilliseconds = 250;
};

/**
 * @brief 文本读取选项。
 *
 * @remarks
 * - 读取接口优先读取 CF_UNICODETEXT，并将 UTF-16 转换为 UTF-8 输出。
 * - 读取成功前不会修改调用方传入的输出字符串。
 * - maxTextBytes 约束最终 UTF-8 文本规模，不把内部 UTF-16 存储和终止 NUL 计入调用方配额。
 */
struct GB_SystemClipboardTextReadOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 读取成功后对输出 UTF-8 文本执行的换行处理策略。 */
    GB_SystemClipboardNewlineMode newlineMode = GB_SystemClipboardNewlineMode::Preserve;

    /** @brief 允许读取的最大文本字节数；默认 256MB，用于防御异常大剪贴板数据。 */
    size_t maxTextBytes = static_cast<size_t>(256) * 1024 * 1024;
};

/**
 * @brief 文本写入选项。
 *
 * @remarks
 * - 写入接口接收 UTF-8 字符串，转换为 Windows 剪贴板标准 Unicode 文本格式 CF_UNICODETEXT。
 * - text 中不允许包含嵌入式 NUL，因为 CF_UNICODETEXT 以终止 NUL 表示字符串结束。
 * - 写入前会先构造完整全局内存块，成功 EmptyClipboard 后再移交给系统剪贴板。
 */
struct GB_SystemClipboardTextWriteOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 写入前对输入 UTF-8 文本执行的换行处理策略。 */
    GB_SystemClipboardNewlineMode newlineMode = GB_SystemClipboardNewlineMode::Preserve;

    /** @brief 是否允许写入空字符串；false 时空字符串会返回 InvalidArgument。 */
    bool allowEmptyText = true;

    /** @brief 允许写入的最大 UTF-8 文本字节数；不包含内部 UTF-16 存储和终止 NUL。 */
    size_t maxTextBytes = static_cast<size_t>(256) * 1024 * 1024;
};

/**
 * @brief 图片读取选项。
 *
 * @remarks
 * - 读取接口会按剪贴板枚举顺序优先尝试真实发布的图片格式，并回退到 CF_DIBV5、CF_DIB、CF_BITMAP。
 * - 输出图像为 GB_Image，当前实现会解码为 8 位 BGRA 四通道图像。
 * - maxImageBytes 既用于限制剪贴板原始图片数据，也用于限制解码后的像素数据规模。
 */
struct GB_SystemClipboardImageReadOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 允许读取的最大图片字节数；默认 512MB，用于防御超大位图导致内存压力。 */
    size_t maxImageBytes = static_cast<size_t>(512) * 1024 * 1024;
};

/**
 * @brief 图片写入选项。
 *
 * @remarks
 * - 写入接口会将输入图像转换为 8 位 BGRA，再写入 DIB 类剪贴板格式。
 * - CF_DIBV5 用于尽量保留 alpha 通道和颜色掩码语义。
 * - 可选 CF_DIB 兼容格式用于增强旧程序或只识别 CF_DIB 的程序的互操作性。
 */
struct GB_SystemClipboardImageWriteOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 允许写入的最大图片字节数；会检查输入图像、转换后像素、DIBV5 与兼容 DIB 数据规模。 */
    size_t maxImageBytes = static_cast<size_t>(512) * 1024 * 1024;

    /** @brief 是否额外发布不携带 alpha 语义的 CF_DIB 兼容格式；透明度仍保留在 CF_DIBV5 中。 */
    bool publishCompatibilityDib = true;
};

/**
 * @brief 文件路径列表读取选项。
 *
 * @remarks
 * - 读取接口读取 CF_HDROP，并通过 DragQueryFileW 获取 Unicode 文件路径列表。
 * - 输出路径统一转换为 UTF-8 字符串。
 * - 读取路径时不校验文件当前是否仍然存在；CF_HDROP 表示“剪贴板中的路径列表”，不是文件系统快照。
 */
struct GB_SystemClipboardFileReadOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 允许读取的最大文件路径数量。 */
    size_t maxFileCount = 65536;

    /** @brief 允许读取的所有路径 UTF-16 字符数累计上限，包含路径终止符和最终双终止符的预算。 */
    size_t maxTotalPathCharacters = static_cast<size_t>(16) * 1024 * 1024;
};

/**
 * @brief 文件路径列表写入选项。
 *
 * @remarks
 * - 写入接口发布 CF_HDROP，路径使用 DROPFILES + UTF-16 双 NUL 终止字符串序列。
 * - 路径只校验 UTF-8、嵌入式 NUL、空字符串和总规模，不强制要求文件真实存在。
 * - 若需要表达“移动”而非“复制”等更复杂 Shell 剪贴板语义，应另行扩展 Preferred DropEffect 等格式。
 */
struct GB_SystemClipboardFileWriteOptions
{
    /** @brief 打开剪贴板时使用的访问重试策略。 */
    GB_SystemClipboardAccessOptions accessOptions;

    /** @brief 允许写入的最大文件路径数量。 */
    size_t maxFileCount = 65536;

    /** @brief 允许写入的所有路径 UTF-16 字符数累计上限，包含路径终止符和最终双终止符的预算。 */
    size_t maxTotalPathCharacters = static_cast<size_t>(16) * 1024 * 1024;
};

/**
 * @brief 剪贴板格式语义分类。
 *
 * @remarks
 * - 该枚举是对 Win32 剪贴板格式 ID 的语义归类，便于上层不直接依赖 Windows.h。
 * - 对标准格式、注册格式、私有格式和 GDI 对象格式分别做粗粒度分类。
 */
enum class GB_SystemClipboardFormatType : uint16_t
{
    /** @brief 未知格式或当前模块未识别的格式。 */
    Unknown = 0,

    /** @brief ANSI/OEM 文本类格式，例如 CF_TEXT 或 CF_OEMTEXT。 */
    Text = 1,

    /** @brief Unicode 文本格式，通常对应 CF_UNICODETEXT。 */
    UnicodeText = 2,

    /** @brief 设备相关位图格式，通常对应 CF_BITMAP。 */
    Bitmap = 3,

    /** @brief 设备无关位图格式，通常对应 CF_DIB。 */
    Dib = 4,

    /** @brief BITMAPV5HEADER 设备无关位图格式，通常对应 CF_DIBV5。 */
    DibV5 = 5,

    /** @brief 文件路径拖放列表格式，通常对应 CF_HDROP。 */
    FilePaths = 6,

    /** @brief HTML 剪贴板格式，通常为注册格式 HTML Format。 */
    Html = 7,

    /** @brief 富文本格式，通常为注册格式 Rich Text Format。 */
    RichText = 8,

    /** @brief 由 RegisterClipboardFormat 注册的应用程序自定义格式。 */
    Registered = 9,

    /** @brief Win32 私有剪贴板格式区间内的格式。 */
    Private = 10,

    /** @brief Win32 GDI 对象剪贴板格式区间内的格式。 */
    GdiObject = 11
};

/**
 * @brief 当前剪贴板中的单个格式信息。
 *
 * @remarks
 * - formatId 保存 Windows 剪贴板格式 ID，但头文件不暴露 Win32 UINT 类型。
 * - formatName 对标准格式使用稳定英文名称，对注册格式使用系统注册名称。
 * - 枚举结果保持系统返回顺序，其中可能包含 Windows 自动合成的兼容格式。
 */
struct GB_SystemClipboardFormatInfo
{
    /** @brief Win32 剪贴板格式 ID；标准格式、注册格式、私有格式和 GDI 对象格式均使用该数值表达。 */
    uint32_t formatId = 0;

    /** @brief 当前格式的语义分类。 */
    GB_SystemClipboardFormatType formatType = GB_SystemClipboardFormatType::Unknown;

    /** @brief 格式名称；标准格式为固定英文名，注册格式为系统注册名，未知格式可能为空或为十六进制描述。 */
    std::string formatName = "";

    /** @brief 是否属于 Win32 标准剪贴板格式。 */
    bool isStandardFormat = false;

    /** @brief 是否属于 RegisterClipboardFormat 注册格式。 */
    bool isRegisteredFormat = false;

    /** @brief 是否属于 CF_PRIVATEFIRST 到 CF_PRIVATELAST 私有格式范围。 */
    bool isPrivateFormat = false;

    /** @brief 是否属于 CF_GDIOBJFIRST 到 CF_GDIOBJLAST GDI 对象格式范围。 */
    bool isGdiObjectFormat = false;
};

/**
 * @brief Windows 系统剪贴板基础能力入口。
 *
 * @remarks
 * - 本类只负责系统剪贴板数据读写、格式查询和序列号读取，不模拟粘贴快捷键，也不管理目标窗口。
 * - 所有 std::string 入参和输出均约定为 UTF-8。
 * - 读取成功前不会修改调用方输出对象；失败时输出对象保持原值或被设置为平台无关的安全默认值。
 * - 读取到的系统句柄和指针会在剪贴板关闭前立即复制或解码，不会暴露给调用方。
 * - 写入接口会先清空剪贴板，再发布新格式；一旦 SetClipboardData 成功，内存句柄所有权移交给系统。
 * - 非 Windows 平台统一返回 UnsupportedPlatform。
 */
class GLOBALBASE_PORT GB_SystemClipboard final
{
public:
    /** @brief 静态工具类，不允许构造实例。 */
    GB_SystemClipboard() = delete;

    /** @brief 静态工具类，不允许析构实例。 */
    ~GB_SystemClipboard() = delete;

    /**
     * @brief 清空当前系统剪贴板。
     *
     * @param options 打开剪贴板时使用的访问重试策略。
     * @return 成功返回 Succeeded；剪贴板被其他进程占用、所有者窗口创建失败或 EmptyClipboard 失败时返回对应错误。
     *
     * @remarks
     * - Windows 下会创建内部隐藏所有者窗口，避免使用空 owner 导致后续 SetClipboardData 语义异常。
     * - 成功清空后会记录当前剪贴板序列号，GB_SystemClipboardWatcher 可据此标记 isSelfWrite。
     */
    static GB_SystemResult Clear(const GB_SystemClipboardAccessOptions& options = GB_SystemClipboardAccessOptions());

    /**
     * @brief 判断指定 Win32 剪贴板格式 ID 当前是否可用。
     *
     * @param formatId Win32 剪贴板格式 ID，不能为 0。
     * @param available 输出是否可用；Windows 下成功返回时一定被写入。
     * @return 参数非法、平台不支持或系统调用失败时返回失败；判断成功返回 Succeeded。
     *
     * @remarks
     * - 该接口不需要打开剪贴板，内部调用 IsClipboardFormatAvailable。
     * - formatId 可以是标准格式 ID，也可以是 RegisterClipboardFormat 得到的注册格式 ID。
     */
    static GB_SystemResult IsFormatAvailable(uint32_t formatId, bool& available);

    /**
     * @brief 判断当前剪贴板是否包含 Unicode 文本。
     *
     * @param hasText 输出是否存在 CF_UNICODETEXT。
     * @return 判断成功返回 Succeeded；非 Windows 平台返回 UnsupportedPlatform。
     */
    static GB_SystemResult HasText(bool& hasText);

    /**
     * @brief 判断当前剪贴板是否包含可尝试读取的图片格式。
     *
     * @param hasImage 输出是否存在 CF_DIBV5、CF_DIB 或 CF_BITMAP 中任意一种格式。
     * @return 判断成功返回 Succeeded；非 Windows 平台返回 UnsupportedPlatform。
     *
     * @remarks
     * - 返回 true 只表示存在候选图片格式，不保证 GetImage 一定能成功解析全部图片内容。
     */
    static GB_SystemResult HasImage(bool& hasImage);

    /**
     * @brief 判断当前剪贴板是否包含文件路径列表。
     *
     * @param hasFilePaths 输出是否存在 CF_HDROP。
     * @return 判断成功返回 Succeeded；非 Windows 平台返回 UnsupportedPlatform。
     */
    static GB_SystemResult HasFilePaths(bool& hasFilePaths);

    /**
     * @brief 获取当前 window station 的剪贴板序列号。
     *
     * @param sequenceNumber 输出剪贴板序列号；无访问权限或非 Windows 平台时可能为 0。
     * @return Windows 下返回 Succeeded；非 Windows 平台返回 UnsupportedPlatform。
     *
     * @remarks
     * - Windows 会在剪贴板内容改变或剪贴板被清空时递增该序列号。
     * - 该接口适合用于缓存失效判断，不建议用高频轮询替代剪贴板监听器。
     */
    static GB_SystemResult GetSequenceNumber(uint64_t& sequenceNumber);

    /**
     * @brief 枚举当前剪贴板可用格式。
     *
     * @param formats 输出格式列表；成功前不会修改调用方原列表。
     * @param options 打开剪贴板时使用的访问重试策略。
     * @return 枚举成功返回 Succeeded；打开剪贴板失败、枚举失败或内存分配失败时返回对应错误。
     *
     * @remarks
     * - 枚举顺序遵循 EnumClipboardFormats 返回顺序，通常先是真实发布格式，再是系统可合成格式。
     * - 输出中每个 GB_SystemClipboardFormatInfo 会包含格式 ID、名称和语义分类。
     */
    static GB_SystemResult GetFormats(std::vector<GB_SystemClipboardFormatInfo>& formats, const GB_SystemClipboardAccessOptions& options = GB_SystemClipboardAccessOptions());

    /**
     * @brief 从系统剪贴板读取 Unicode 文本并输出为 UTF-8。
     *
     * @param text 输出文本；成功前不会修改调用方原字符串。
     * @param options 文本读取选项。
     * @return 成功返回 Succeeded；无文本、编码转换失败、数据不合法或超过大小限制时返回对应错误。
     *
     * @remarks
     * - 只读取 CF_UNICODETEXT，不主动读取 CF_TEXT 或 CF_OEMTEXT。
     * - 会校验剪贴板内存块大小、UTF-16 终止 NUL，并按 newlineMode 规范化输出换行。
     */
    static GB_SystemResult GetText(std::string& text, const GB_SystemClipboardTextReadOptions& options = GB_SystemClipboardTextReadOptions());

    /**
     * @brief 将 UTF-8 文本写入系统剪贴板。
     *
     * @param text 输入文本，必须是有效 UTF-8，且不能包含嵌入式 NUL。
     * @param options 文本写入选项。
     * @return 成功返回 Succeeded；参数非法、编码转换失败、剪贴板打开失败或写入失败时返回对应错误。
     *
     * @remarks
     * - 内部写入格式为 CF_UNICODETEXT。
     * - 若 newlineMode 非 Preserve，会先规范化换行再转换为 UTF-16。
     * - 成功写入后会记录当前剪贴板序列号，GB_SystemClipboardWatcher 可据此标记 isSelfWrite。
     */
    static GB_SystemResult SetText(const std::string& text, const GB_SystemClipboardTextWriteOptions& options = GB_SystemClipboardTextWriteOptions());

    /**
     * @brief 读取剪贴板图片。
     *
     * @param image 输出图像；成功前不会修改调用方原图像。
     * @param options 图片读取选项。
     * @return 成功返回 Succeeded；无图片格式、图片格式无法解析、数据超过限制或平台不支持时返回对应错误。
     *
     * @remarks
     * - 按系统枚举顺序优先解析真实发布的图片格式，并回退到可用的 CF_DIBV5 / CF_DIB / CF_BITMAP。
     * - 当前实现支持常见 1/4/8/16/24/32 位 DIB、位掩码 DIB、DIBV5 以及 CF_BITMAP 解码路径。
     * - 解析成功后输出通常为 8 位 BGRA 四通道图像；透明度缺失或全 0 alpha 的部分传统 DIB 会按不透明处理。
     */
    static GB_SystemResult GetImage(GB_Image& image, const GB_SystemClipboardImageReadOptions& options = GB_SystemClipboardImageReadOptions());

    /**
     * @brief 写入剪贴板图片。
     *
     * @param image 输入图像；必须非空，并且可以转换为 8 位 BGRA 图像。
     * @param options 图片写入选项。
     * @return 成功返回 Succeeded；图像非法、超过大小限制、剪贴板打开失败或 SetClipboardData 失败时返回对应错误。
     *
     * @remarks
     * - 默认发布保留 alpha 的 CF_DIBV5。
     * - publishCompatibilityDib=true 时额外发布不携带 alpha 语义的 CF_DIB 兼容格式，以提高旧程序互操作性。
     * - 成功写入后会记录当前剪贴板序列号，GB_SystemClipboardWatcher 可据此标记 isSelfWrite。
     */
    static GB_SystemResult SetImage(const GB_Image& image, const GB_SystemClipboardImageWriteOptions& options = GB_SystemClipboardImageWriteOptions());

    /**
     * @brief 从剪贴板读取文件路径列表。
     *
     * @param filePaths 输出 UTF-8 文件路径列表；成功前不会修改调用方原列表。
     * @param options 文件路径列表读取选项。
     * @return 成功返回 Succeeded；无 CF_HDROP、数量或总长度超限、路径转换失败时返回对应错误。
     *
     * @remarks
     * - 内部读取 CF_HDROP，并使用 DragQueryFileW 获取路径。
     * - 输出路径是否存在由调用方自行判断，本接口只反映剪贴板数据。
     */
    static GB_SystemResult GetFilePaths(std::vector<std::string>& filePaths, const GB_SystemClipboardFileReadOptions& options = GB_SystemClipboardFileReadOptions());

    /**
     * @brief 将文件路径列表写入剪贴板。
     *
     * @param filePaths 输入 UTF-8 文件路径列表；不能为空，路径项不能为空且不能包含嵌入式 NUL。
     * @param options 文件路径列表写入选项。
     * @return 成功返回 Succeeded；参数非法、编码转换失败、内存分配失败或剪贴板写入失败时返回对应错误。
     *
     * @remarks
     * - 内部发布 CF_HDROP，路径使用 UTF-16 宽字符双 NUL 终止列表。
     * - 不强制要求路径真实存在，便于上层表达虚拟路径、延迟生成路径或测试数据。
     * - 成功写入后会记录当前剪贴板序列号，GB_SystemClipboardWatcher 可据此标记 isSelfWrite。
     */
    static GB_SystemResult SetFilePaths(const std::vector<std::string>& filePaths, const GB_SystemClipboardFileWriteOptions& options = GB_SystemClipboardFileWriteOptions());

    /**
     * @brief 获取剪贴板格式分类的稳定英文名称。
     *
     * @param formatType 剪贴板格式语义分类。
     * @return Unknown、Text、UnicodeText、Bitmap、Dib、DibV5、FilePaths、Html、RichText、Registered、Private 或 GdiObject。
     */
    static std::string GetFormatTypeName(GB_SystemClipboardFormatType formatType);
};

/**
 * @brief 剪贴板变化事件。
 *
 * @remarks
 * - eventName 固定为 "SystemClipboard.Changed"。
 * - sequenceNumber 是收到 WM_CLIPBOARDUPDATE 时捕获的当前 window station 剪贴板序列号。
 * - formatsConsistent=true 表示枚举格式前后序列号均与事件序列号相同。
 * - isSelfWrite 表示该序列号由当前进程内 GB_SystemClipboard 的成功写入或清空产生。
 * - 事件不携带文本、图片、文件路径等完整内容，避免敏感信息泄漏和大对象复制。
 */
struct GB_SystemClipboardEvent
{
    /** @brief 事件名称，固定为 "SystemClipboard.Changed"，便于通用事件总线订阅。 */
    std::string eventName = "SystemClipboard.Changed";

    /** @brief 事件触发时读取到的剪贴板序列号；0 可能表示无 window station 剪贴板访问权限。 */
    uint64_t sequenceNumber = 0;

    /** @brief 事件生成时间戳，单位为毫秒，来源于 GB_EventDispatcher::GetCurrentTimestampMilliseconds()。 */
    uint64_t timestampMilliseconds = 0;

    /** @brief 是否由当前进程内 GB_SystemClipboard 成功写入或清空触发。 */
    bool isSelfWrite = false;

    /** @brief formats 是否与 sequenceNumber 对应的同一剪贴板状态一致。 */
    bool formatsConsistent = false;

    /** @brief 事件发生时枚举到的剪贴板格式；仅当监听选项 includeFormats=true 且枚举成功时填充。 */
    std::vector<GB_SystemClipboardFormatInfo> formats;
};

/**
 * @brief 剪贴板变化监听器配置。
 *
 * @remarks
 * - 配置在构造 GB_SystemClipboardWatcher 时传入，当前接口不支持运行中动态修改。
 * - maxQueueSize 同时用于内部事件队列和 GB_EventDispatcher 队列的容量语义。
 */
struct GB_SystemClipboardWatcherOptions
{
    /** @brief 内部待处理剪贴板事件队列最大长度；0 表示不限制，非 0 时队列满会丢弃最旧事件。 */
    size_t maxQueueSize = 256;

    /** @brief 是否在事件中附带当前剪贴板格式列表；false 可减少事件处理开销和剪贴板占用时间。 */
    bool includeFormats = true;

    /** @brief includeFormats=true 时枚举格式所使用的剪贴板访问重试策略。 */
    GB_SystemClipboardAccessOptions formatAccessOptions;
};

/**
 * @brief Windows 系统剪贴板变化监听器。
 *
 * @remarks
 * - 监听线程只创建隐藏消息窗口并接收 WM_CLIPBOARDUPDATE。
 * - 监听线程只负责接收系统消息，格式枚举、强类型回调和通用事件投递会转交内部工作线程处理。
 * - Start()/Stop() 可重复调用，析构时自动停止。
 * - 回调中可以调用 Stop()，但不应在回调返回前销毁监听器实例。
 * - 若在回调线程内调用 Stop()，内部事件线程会在回调返回后退出，后续从其他线程再次调用 Stop()/Start() 会完成线程回收。
 * - 回调中应避免执行长时间阻塞操作。
 * - 非 Windows 平台 Start()/Stop() 返回 UnsupportedPlatform，IsRunning() 返回 false。
 */
class GLOBALBASE_PORT GB_SystemClipboardWatcher final
{
public:
    /**
     * @brief 剪贴板变化事件回调函数类型。
     *
     * @param event 剪贴板变化事件对象；生命周期仅限本次回调，若需异步使用应自行拷贝。
     *
     * @remarks
     * - 回调在内部事件工作线程执行，不在创建监听器的调用线程执行。
     * - 回调异常会被内部捕获并吞掉，以避免中断监听线程。
     */
    using ClipboardEventCallback = std::function<void(const GB_SystemClipboardEvent& event)>;

    /**
     * @brief 使用默认选项构造剪贴板监听器。
     *
     * @remarks 构造函数不会启动监听；需要显式调用 Start()。
     */
    GB_SystemClipboardWatcher();

    /**
     * @brief 使用指定选项构造剪贴板监听器。
     *
     * @param options 监听器配置，会被复制保存到内部实现中。
     *
     * @remarks 构造函数不会启动监听；需要显式调用 Start()。
     */
    explicit GB_SystemClipboardWatcher(const GB_SystemClipboardWatcherOptions& options);

    /**
     * @brief 析构监听器并自动停止监听。
     *
     * @remarks 析构函数 noexcept；内部停止失败不会向外抛出异常。
     */
    ~GB_SystemClipboardWatcher() noexcept;

    /** @brief 监听器不可拷贝，避免隐藏窗口、线程和回调状态出现双重所有权。 */
    GB_SystemClipboardWatcher(const GB_SystemClipboardWatcher&) = delete;

    /** @brief 监听器不可拷贝赋值，避免隐藏窗口、线程和回调状态出现双重所有权。 */
    GB_SystemClipboardWatcher& operator=(const GB_SystemClipboardWatcher&) = delete;

    /**
     * @brief 启动剪贴板变化监听。
     *
     * @return 成功启动或已经处于运行状态时返回 Succeeded；创建线程、创建隐藏窗口、注册监听失败时返回对应错误。
     *
     * @remarks
     * - Windows 下内部使用 AddClipboardFormatListener 注册隐藏消息窗口。
     * - 若上一次 Stop() 在回调线程内触发导致线程尚未回收，再次 Start() 会先尝试回收旧线程。
     * - 不应在监听线程或事件回调线程内部重新启动同一个监听器。
     */
    GB_SystemResult Start();

    /**
     * @brief 停止剪贴板变化监听。
     *
     * @return 成功停止或原本未运行时返回 Succeeded；停止消息投递失败或事件派发器停止失败时返回对应错误。
     *
     * @remarks
     * - Stop() 会尽量处理完已入队事件，再停止通用事件派发器。
     * - 可以在 ClipboardEventCallback 中调用 Stop()；此时内部避免 join 当前事件线程。
     */
    GB_SystemResult Stop();

    /**
     * @brief 判断监听器当前是否处于运行状态。
     *
     * @return true 表示隐藏监听窗口和事件线程已成功启动；false 表示未启动或已停止。
     */
    bool IsRunning() const;

    /**
     * @brief 设置强类型剪贴板事件回调。
     *
     * @param callback 回调函数；传入空 std::function 可清除回调。
     *
     * @remarks
     * - 可在 Start() 前或 Start() 后调用。
     * - 回调对象会被复制保存；回调执行期间内部会先复制一份再调用，避免持锁执行用户代码。
     */
    void SetClipboardEventCallback(const ClipboardEventCallback& callback);

    /**
     * @brief 获取通用事件派发器。
     *
     * @return 内部 GB_EventDispatcher 引用。
     *
     * @remarks
     * - 除强类型回调外，监听器还会向该派发器投递名称为 "SystemClipboard.Changed" 的 GB_Event。
     * - 通用事件不携带完整剪贴板内容，仅用于系统事件订阅/转发场景。
     */
    GB_EventDispatcher& GetEventDispatcher();

private:
    /** @brief PImpl 实现，隐藏 Windows.h、窗口句柄、线程和同步细节，降低头文件污染。 */
    class Impl;

    /** @brief 内部实现对象唯一所有权。 */
    std::unique_ptr<Impl> impl;
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif // GLOBALBASE_SYSTEM_CLIPBOARD_H_H
