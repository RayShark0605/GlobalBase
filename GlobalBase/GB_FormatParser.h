#ifndef GLOBALBASE_FORMAT_PARSER_H_H
#define GLOBALBASE_FORMAT_PARSER_H_H

#include "GlobalBasePort.h"

#include "GB_BaseTypes.h"

#include "GB_Variant.h"

#include <string>
#include <vector>

/**
 * @brief JSON 解析器。
 */
class GLOBALBASE_PORT GB_JsonParser
{
public:
    /**
     * @brief 将 JSON 文本解析为 GB_Variant。
     *
     * 支持的 JSON 节点类型会按如下方式转换：
     * - null -> 空的 GB_Variant
     * - boolean -> bool
     * - string -> std::string
     * - integer / long -> 整型
     * - double -> double
     * - object -> GB_VariantMap
     * - array -> GB_VariantList
     *
     * @param jsonText 输入的 JSON 文本（UTF-8 / ASCII 均可）。
     * @param[out] outValue 解析成功后输出的结果。
     * @param[out] errorMessage 可选的错误信息输出；成功时会被清空。
     * @return true 表示解析并转换成功；false 表示失败。
     */
    static bool ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage = nullptr);

    /**
     * @brief 将 JSON 文本解析为 GB_VariantMap。
     *
     * 与 ParseToVariant() 的区别在于：本接口要求 JSON 根节点必须是 object。
     * 若根节点不是 object，则返回 false。
     *
     * @param jsonText 输入的 JSON 文本（UTF-8 / ASCII 均可）。
     * @param[out] outMap 解析成功后输出的键值映射结果。
     * @param[out] errorMessage 可选的错误信息输出；成功时会被清空。
     * @return true 表示解析成功且根节点为 object；false 表示失败。
     */
    static bool ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage = nullptr);
};

/**
 * @brief XML 属性信息。
 *
 * 用于描述单个属性的完整语义，包括：
 * - attributeName：属性完整名字，可能包含前缀，例如 "xlink:href"
 * - localName：属性局部名字，不包含命名空间前缀
 * - namespacePrefix：属性所属命名空间前缀
 * - namespaceUri：属性所属命名空间 URI
 * - attributeValue：属性值
 */
struct GB_XmlAttribute
{
    std::string attributeName = "";
    std::string localName = "";
    std::string namespacePrefix = "";
    std::string namespaceUri = "";
    std::string attributeValue = "";
};

/**
 * @brief XML 命名空间声明信息。
 *
 * 用于保存节点上的 xmlns 声明项。例如：
 * - xmlns="http://www.opengis.net/ows"
 * - xmlns:ows="http://www.opengis.net/ows"
 */
struct GB_XmlNamespaceDeclaration
{
    std::string namespacePrefix = "";
    std::string namespaceUri = "";
};

/**
 * @brief XML 诊断信息。
 */
struct GB_XmlDiagnostic
{
    /**
     * @brief 诊断级别。
     */
    enum class Level
    {
        Warning,
        Error,
        Fatal
    };

    Level level = Level::Error;
    int code = 0;
    long long lineNumber = -1;
    int columnNumber = -1;
    std::string message = "";
};

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

/**
 * @brief XML 节点。
 *
 * 该结构用于描述解析后的 XML 树节点。不同 nodeType 下，各字段的使用方式如下：
 * - Element：nodeTag / localName / namespace* / attributes / children 有意义
 * - Text / CData / Comment：nodeValue 有意义
 * - ProcessingInstruction / EntityReference：nodeTag 与 nodeValue / children 视情况有效
 *
 * 对于元素节点：
 * - nodeTag 尽量保留原始限定名，例如 "ows:Title"
 * - localName 为本地名，例如 "Title"
 * - GetValue() 可递归提取当前节点下的文本值
 */
struct GLOBALBASE_PORT GB_XmlNode
{
    /**
     * @brief 节点类型。
     */
    enum class Type
    {
        Element,
        Text,
        CData,
        Comment,
        ProcessingInstruction,
        EntityReference
    };

    Type nodeType = Type::Element;
    std::string nodeTag = "";
    std::string localName = "";
    std::string namespacePrefix = "";
    std::string namespaceUri = "";
    std::string nodeValue = "";
    long long lineNumber = -1;
    std::vector<GB_XmlAttribute> attributes;
    std::vector<GB_XmlNamespaceDeclaration> namespaceDeclarations;
    std::vector<GB_XmlNode> children;

    /**
     * @brief 判断当前节点是否为指定名称的元素节点。
     *
     * 匹配时会优先比较完整名称 nodeTag，同时也支持使用 localName 匹配。
     * 例如当前节点 tag 为 "ows:Title" 时，传入 "ows:Title" 或 "Title" 都可以匹配。
     *
     * @param elementName 待匹配的元素名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return true 表示当前节点是匹配的元素节点；否则返回 false。
     */
    bool IsElement(const std::string& elementName, bool caseSensitive = false) const;

    /**
     * @brief 判断当前元素节点是否包含指定属性。
     *
     * 匹配规则同时支持完整名和 localName。例如属性名为 "xlink:href" 时，
     * 传入 "xlink:href" 或 "href" 都可以命中。
     *
     * @param attributeName 属性名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return true 表示存在该属性；否则返回 false。
     */
    bool HasAttribute(const std::string& attributeName, bool caseSensitive = false) const;

    /**
     * @brief 获取指定属性的只读指针。
     *
     * @param attributeName 属性名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 找到时返回属性指针；找不到时返回 nullptr。
     */
    const GB_XmlAttribute* GetAttribute(const std::string& attributeName, bool caseSensitive = false) const;

    /**
     * @brief 获取指定属性的可写指针。
     *
     * @param attributeName 属性名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 找到时返回属性指针；找不到时返回 nullptr。
     */
    GB_XmlAttribute* GetAttribute(const std::string& attributeName, bool caseSensitive = false);

    /**
     * @brief 尝试获取指定属性的属性值。
     *
     * @param attributeName 属性名称。
     * @param[out] outValue 找到时输出属性值。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return true 表示成功获取；false 表示未找到。
     */
    bool TryGetAttributeValue(const std::string& attributeName, std::string& outValue, bool caseSensitive = false) const;

    /**
     * @brief 获取指定属性的属性值。
     *
     * 若属性不存在，则返回空字符串。
     *
     * @param attributeName 属性名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 属性值；若不存在则返回空字符串。
     */
    std::string GetAttributeValue(const std::string& attributeName, bool caseSensitive = false) const;

    /**
     * @brief 判断当前节点是否包含指定名称的子元素节点。
     *
     * 仅在 children 中查找元素类型节点，不会匹配文本、注释等其它节点。
     *
     * @param childName 子元素名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return true 表示存在匹配的子元素；否则返回 false。
     */
    bool HasChild(const std::string& childName, bool caseSensitive = false) const;

    /**
     * @brief 获取第一个匹配名称的子元素节点（只读）。
     *
     * 仅返回第一个命中的元素子节点。
     *
     * @param childName 子元素名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 找到时返回子节点指针；否则返回 nullptr。
     */
    const GB_XmlNode* GetChild(const std::string& childName, bool caseSensitive = false) const;

    /**
     * @brief 获取第一个匹配名称的子元素节点（可写）。
     *
     * @param childName 子元素名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 找到时返回子节点指针；否则返回 nullptr。
     */
    GB_XmlNode* GetChild(const std::string& childName, bool caseSensitive = false);

    /**
     * @brief 获取所有匹配名称的子元素节点（只读）。
     *
     * 当 childName 为空时，返回当前节点下的全部元素类型子节点。
     *
     * @param childName 子元素名称；为空时表示不过滤名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 匹配到的子元素节点指针数组。
     */
    std::vector<const GB_XmlNode*> GetChildren(const std::string& childName = "", bool caseSensitive = false) const;

    /**
     * @brief 获取所有匹配名称的子元素节点（可写）。
     *
     * 当 childName 为空时，返回当前节点下的全部元素类型子节点。
     *
     * @param childName 子元素名称；为空时表示不过滤名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 匹配到的子元素节点指针数组。
     */
    std::vector<GB_XmlNode*> GetChildren(const std::string& childName = "", bool caseSensitive = false);

    /**
     * @brief 尝试获取指定子元素的文本值。
     *
     * 实际返回的是该子元素调用 GetValue() 的结果，因此会递归拼接其下属的文本 / CDATA。
     *
     * @param childName 子元素名称。
     * @param[out] outValue 找到时输出该子元素的文本值。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return true 表示找到并成功输出；false 表示未找到。
     */
    bool TryGetChildValue(const std::string& childName, std::string& outValue, bool caseSensitive = false) const;

    /**
     * @brief 获取指定子元素的文本值。
     *
     * 若不存在匹配的子元素，则返回空字符串。
     *
     * @param childName 子元素名称。
     * @param caseSensitive 是否区分大小写；默认 false。
     * @return 第一个匹配子元素的文本值；若未找到则返回空字符串。
     */
    std::string GetChildValue(const std::string& childName, bool caseSensitive = false) const;

    /**
     * @brief 获取当前节点的文本值。
     *
     * 规则如下：
     * - Text / CData：直接返回 nodeValue
     * - Element / EntityReference：递归拼接子节点中的文本 / CDATA
     * - Comment / ProcessingInstruction：直接返回 nodeValue
     *
     * 对于类似 `<ows:Title>标题</ows:Title>` 的节点，
     * 调用 GetValue() 可直接得到 `"标题"`。
     *
     * @return 当前节点对应的文本值。
     */
    std::string GetValue() const;
};

/**
 * @brief XML 文档。
 *
 * 用于完整表示一个解析后的 XML 文档，包括：
 * - XML 声明信息（version / encoding / standalone）
 * - 文档类型信息（DOCTYPE）
 * - 解析诊断信息
 * - 根节点之前的节点（prologNodes）
 * - 根节点（rootNode）
 * - 根节点之后的节点（epilogNodes）
 */
struct GB_XmlDocument
{
    /**
     * @brief XML 声明中的 standalone 状态。
     */
    enum class StandaloneMode
    {
        NoDeclaration = -1,
        Omitted = -2,
        No = 0,
        Yes = 1
    };

    std::string version = "";
    std::string encoding = "";
    StandaloneMode standalone = StandaloneMode::NoDeclaration;
    std::string documentTypeName = "";
    std::string documentTypePublicId = "";
    std::string documentTypeSystemId = "";
    bool hasInternalSubset = false;
    bool hasExternalSubset = false;
    bool recovered = false;
    std::vector<GB_XmlDiagnostic> diagnostics;
    std::vector<GB_XmlNode> prologNodes;
    GB_XmlNode rootNode;
    std::vector<GB_XmlNode> epilogNodes;
};

/**
 * @brief XML 解析选项。
 *
 * 该结构主要用于控制解析器的容错、安全策略、空白保留策略、命名空间信息保留策略。
 */
struct GB_XmlParserOptions
{
    /**
     * @brief 是否允许恢复模式。
     *
     * 为 true 时，解析器会尽量从不完全规范的 XML 中恢复结构。
     */
    bool allowRecovery = false;

    /**
     * @brief 是否保留纯空白文本节点。
     *
     * 为 false 时，形如缩进、换行产生的空白 Text 节点通常会被忽略。
     */
    bool preserveWhitespaceOnlyTextNodes = false;

    /**
     * @brief 是否保留 CDATA 为独立节点。
     *
     * 为 true 时，CDATA 会作为 Type::CData 节点保留；
     * 为 false 时，libxml2 可能把它折叠为普通文本。
     */
    bool preserveCDataSections = true;

    /**
     * @brief 是否保留注释节点。
     */
    bool preserveComments = true;

    /**
     * @brief 是否保留处理指令节点。
     */
    bool preserveProcessingInstructions = true;

    /**
     * @brief 是否保留实体引用节点。
     */
    bool preserveEntityReferences = true;

    /**
     * @brief 是否输出节点上的命名空间声明集合。
     */
    bool includeNamespaceDeclarations = true;

    /**
     * @brief 是否清理冗余命名空间声明。
     */
    bool cleanRedundantNamespaceDeclarations = false;

    /**
     * @brief 是否替换实体引用。
     */
    bool substituteEntities = false;

    /**
     * @brief 是否加载外部 DTD。
     */
    bool loadExternalDtd = false;

    /**
     * @brief 是否应用 DTD 中声明的默认属性。
     */
    bool applyDefaultDtdAttributes = false;

    /**
     * @brief 是否按 DTD 进行校验。
     */
    bool validateWithDtd = false;

    /**
     * @brief 是否允许外部实体。
     *
     * 出于安全考虑，默认关闭。
     */
    bool allowExternalEntities = false;

    /**
     * @brief 是否允许网络访问。
     *
     * 出于安全与可控性考虑，默认关闭。
     */
    bool allowNetworkAccess = false;

    /**
     * @brief 是否允许超大文档模式。
     */
    bool allowHugeDocuments = false;

    /**
     * @brief 是否启用紧凑内存模式。
     */
    bool compactMemory = true;

    /**
     * @brief 是否启用较大行号报告支持。
     */
    bool reportLargeLineNumbers = true;

    /**
     * @brief 文档基准 URL。
     *
     * 可用于错误报告及相对外部引用的解析上下文。
     */
    std::string baseUrl = "";

    /**
     * @brief 强制指定输入编码。
     *
     * 为空时自行探测。
     */
    std::string forcedEncoding = "";
};

/**
 * @brief XML 解析器。
 *
 * 将 XML 文本解析为 GB_XmlDocument 或 GB_XmlNode。
 * 其设计目标偏工程实用主义，兼顾：
 * - 节点结构的完整表达
 * - 命名空间信息保留
 * - 错误诊断可读性
 * - 解析安全性
 */
class GLOBALBASE_PORT GB_XmlParser
{
public:
    /**
     * @brief 将 XML 文本解析为完整文档对象。
     *
     * 输出结果中除根节点外，还会包含 XML 声明、DOCTYPE、诊断信息，以及
     * 根节点前后的其它顶层节点（如注释、处理指令等）。
     *
     * @param xmlText 输入 XML 文本。
     * @param[out] outDocument 解析成功后输出的文档结构。
     * @param options 解析选项。
     * @param[out] errorMessage 可选的错误信息输出；成功时会被清空。
     * @return true 表示解析成功；false 表示失败。
     */
    static bool ParseToDocument(const std::string& xmlText, GB_XmlDocument& outDocument, const GB_XmlParserOptions& options = GB_XmlParserOptions(), std::string* errorMessage = nullptr);

    /**
     * @brief 将 XML 文本解析为根节点。
     *
     * 该接口适用于调用方只关心根元素及其子树，不关心 XML 声明、DOCTYPE、
     * prolog / epilog 等文档级信息的场景。
     *
     * @param xmlText 输入 XML 文本。
     * @param[out] outRootNode 解析成功后输出根元素节点。
     * @param options 解析选项。
     * @param[out] errorMessage 可选的错误信息输出；成功时会被清空。
     * @return true 表示解析成功；false 表示失败。
     */
    static bool ParseToRootNode(const std::string& xmlText, GB_XmlNode& outRootNode, const GB_XmlParserOptions& options = GB_XmlParserOptions(), std::string* errorMessage = nullptr);
};

#ifdef _MSC_VER
# pragma warning(pop)
#endif

#endif
