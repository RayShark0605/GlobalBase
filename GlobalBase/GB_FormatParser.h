#ifndef GLOBALBASE_FORMAT_PARSER_H_H
#define GLOBALBASE_FORMAT_PARSER_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include "GB_Variant.h"

#include <cstddef>
#include <string>
#include <vector>

/**
 * @brief JSON 解析器。
 *
 * 基于 GDAL 的 CPLJSONDocument / CPLJSONObject，把 JSON 文本转换为
 * GB_Variant / GB_VariantMap / GB_VariantList 组合而成的树状结构。
 */
class GLOBALBASE_PORT GB_JsonParser
{
public:
    /**
     * @brief 把 JSON 文本解析为 GB_Variant。
     *
     * 成功时，outValue 会被赋值为 JSON 根节点对应的 Variant；失败时 outValue 保持原值。
     */
    static bool ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage = nullptr);

    /**
     * @brief 把 JSON 文本解析为 GB_VariantMap。
     *
     * 该接口要求 JSON 根节点必须是 object；失败时 outMap 保持原值。
     */
    static bool ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage = nullptr);
};

/**
 * @brief XML 解析器。
 *
 * 设计目标：
 * - 基于 libxml2 解析内存中的 XML 字符串。
 * - 结果统一输出为 GB_Variant / GB_VariantMap / GB_VariantList 组合而成的树结构。
 * - 默认兼顾安全性、通用性与工程可用性；对需要更强容错、DTD、实体、空白保留等场景，
 *   通过 Options 进行显式控制。
 *
 * 输出结构说明：
 * - OutputMode::Document：输出一个 document 节点对象。
 * - OutputMode::RootElement：直接输出根元素节点对象。
 *
 * 元素节点常用字段：
 * - nodeType: 固定为 "element"
 * - name（尽量保留前缀后的限定名）/ localName（本地名）
 * - namespacePrefix / namespaceUri（按选项决定是否输出）
 * - attributes: 属性集合
 * - namespaceDeclarations: 当前节点声明的命名空间集合（按选项决定是否输出）
 * - children: 子节点列表
 * - directText: 当前元素直属文本内容拼接结果
 * - line: 行号（按选项决定是否输出）
 *
 * 文档节点常用字段：
 * - nodeType: 固定为 "document"
 * - version / encoding / standalone / url（按选项决定是否输出）
 * - doctype: 文档类型信息（按选项决定是否输出）
 * - children: 文档级子节点列表
 * - rootElementIndex: 根元素在 children 中的下标
 */
class GLOBALBASE_PORT GB_XmlParser
{
public:
    /**
     * @brief 解析结果的输出模式。
     */
    enum class OutputMode
    {
        Document,
        RootElement
    };

    /**
     * @brief 属性的输出模式。
     */
    enum class AttributeOutputMode
    {
        ValueOnly,
        RichObject
    };

    /**
     * @brief 诊断信息。
     */
    struct Diagnostic
    {
        int level = 0;
        int code = 0;
        int line = 0;
        int column = 0;
        std::string file;
        std::string message;
    };

    /**
     * @brief XML 解析选项。
     */
    struct Options
    {
        /** 输出整个 document 节点，还是只输出根元素节点。 */
        OutputMode outputMode = OutputMode::Document;

        /** attributes 字段是直接输出字符串值，还是输出富对象。 */
        AttributeOutputMode attributeOutputMode = AttributeOutputMode::ValueOnly;

        /** 是否允许 libxml2 recovery 模式。默认关闭，以严格正确性为先。 */
        bool allowRecovery = false;

        /** 是否允许网络访问。默认关闭。 */
        bool allowNetwork = false;

        /** 是否允许外部实体/外部 DTD。默认关闭。 */
        bool allowExternalEntities = false;

        /** 是否展开实体。默认关闭。 */
        bool expandEntities = false;

        /** 是否加载外部 DTD。默认关闭。 */
        bool loadExternalDtd = false;

        /** 是否应用 DTD 中声明的默认属性。默认关闭。 */
        bool applyDefaultDtdAttributes = false;

        /** 是否进行 DTD 校验。默认关闭。 */
        bool validateDtd = false;

        /** 是否保留注释节点。默认关闭。 */
        bool preserveComments = false;

        /** 是否保留处理指令节点。默认关闭。 */
        bool preserveProcessingInstructions = false;

        /** 是否把 CDATA 保留为独立节点。默认关闭，转为普通文本。 */
        bool preserveCDataSections = false;

        /** 是否保留纯空白文本节点。默认关闭。 */
        bool preserveBlankText = false;

        /** 是否对文本节点做首尾裁剪。默认关闭。 */
        bool trimText = false;

        /** trim 后若文本为空，是否跳过该文本节点。默认开启。 */
        bool skipEmptyTextAfterTrim = true;

        /** 是否合并相邻的同类文本节点。默认开启。 */
        bool mergeAdjacentText = true;

        /** 是否输出 XML 声明相关字段（version / encoding / standalone）。默认开启。 */
        bool includeDeclaration = true;

        /** 是否输出文档类型信息。默认开启。 */
        bool includeDoctype = true;

        /** 是否输出命名空间前缀与 URI 信息。默认开启。 */
        bool includeNamespaceInfo = true;

        /** 是否输出当前节点声明的命名空间集合。默认关闭。 */
        bool includeNamespaceDeclarations = false;

        /** 是否输出节点行号。默认关闭。 */
        bool includeNodeLineNumbers = false;

        /** 是否启用 pedantic 模式。默认关闭。 */
        bool pedantic = false;

        /** 是否请求 libxml2 清理冗余命名空间声明。默认开启。 */
        bool cleanupRedundantNamespaces = true;

        /** 是否启用 libxml2 的 compact 存储模式。默认关闭。 */
        bool compactMemory = false;

        /** 是否启用 huge 模式以放宽内部限制。默认关闭。 */
        bool useHugeMode = false;

        /** 是否忽略 XML 声明中的编码信息。默认关闭。 */
        bool ignoreEncodingDeclaration = false;

        /** 解析时使用的 base URL，可用于错误信息及相对引用解析。 */
        std::string baseUrl;

        /** 可选编码提示；为空时由 libxml2 自行判定。 */
        std::string encodingHint;

        /** 最多收集多少条诊断信息。 */
        std::size_t maxDiagnosticCount = 32;
    };

public:
    /**
     * @brief 把 XML 文本解析为 GB_Variant。
     *
     * 成功时，outValue 会被赋值为 document 节点或根元素节点；失败时 outValue 保持原值。
     */
    static bool ParseToVariant(const std::string& xmlText, GB_Variant& outValue, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    static bool ParseToVariant(const std::string& xmlText, GB_Variant& outValue, const Options& options, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    /**
     * @brief 把 XML 文本解析为 GB_VariantMap。
     *
     * 该接口要求最终结果必须是一个 map 结构；失败时 outMap 保持原值。
     */
    static bool ParseToVariantMap(const std::string& xmlText, GB_VariantMap& outMap, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    static bool ParseToVariantMap(const std::string& xmlText, GB_VariantMap& outMap, const Options& options, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    /**
     * @brief 直接解析根元素为 GB_Variant。
     */
    static bool ParseRootElementToVariant(const std::string& xmlText, GB_Variant& outValue, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    static bool ParseRootElementToVariant(const std::string& xmlText, GB_Variant& outValue, const Options& options, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    /**
     * @brief 直接解析根元素为 GB_VariantMap。
     */
    static bool ParseRootElementToVariantMap(const std::string& xmlText, GB_VariantMap& outMap, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);

    static bool ParseRootElementToVariantMap(const std::string& xmlText, GB_VariantMap& outMap, const Options& options, std::string* errorMessage = nullptr, std::vector<Diagnostic>* diagnostics = nullptr);
};

#endif
