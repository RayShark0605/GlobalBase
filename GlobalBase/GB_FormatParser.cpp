#include "GB_FormatParser.h"

#include "cpl_json.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>

namespace
{
    using XmlOptions = GB_XmlParser::Options;
    using XmlDiagnostic = GB_XmlParser::Diagnostic;

    struct XmlParserCtxtDeleter
    {
        void operator()(xmlParserCtxtPtr parserContext) const
        {
            if (parserContext != nullptr)
            {
                xmlFreeParserCtxt(parserContext);
            }
        }
    };

    struct XmlDocDeleter
    {
        void operator()(xmlDocPtr document) const
        {
            if (document != nullptr)
            {
                xmlFreeDoc(document);
            }
        }
    };

    struct XmlCharDeleter
    {
        void operator()(xmlChar* text) const
        {
            if (text != nullptr)
            {
                xmlFree(text);
            }
        }
    };

    using UniqueXmlParserContext = std::unique_ptr<xmlParserCtxt, XmlParserCtxtDeleter>;
    using UniqueXmlDocument = std::unique_ptr<xmlDoc, XmlDocDeleter>;
    using UniqueXmlChar = std::unique_ptr<xmlChar, XmlCharDeleter>;

    void EnsureXmlLibraryInitialized()
    {
        static std::once_flag initFlag;
        std::call_once(initFlag, []()
            {
                xmlInitParser();
            });
    }

    void ClearOptionalErrorMessage(std::string* errorMessage)
    {
        if (errorMessage != nullptr)
        {
            errorMessage->clear();
        }
    }

    void AssignOptionalErrorMessage(std::string* errorMessage, const std::string& message)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
    }

    void ClearOptionalDiagnostics(std::vector<XmlDiagnostic>* diagnostics)
    {
        if (diagnostics != nullptr)
        {
            diagnostics->clear();
        }
    }

    void InsertOrAssignVariantMapValue(GB_VariantMap& variantMap, const std::string& key, GB_Variant&& value)
    {
        GB_VariantMap::iterator iter = variantMap.lower_bound(key);
        if (iter != variantMap.end() && iter->first == key)
        {
            iter->second = std::move(value);
            return;
        }

        variantMap.emplace_hint(iter, key, std::move(value));
    }

    void InsertOrAssignVariantMapString(GB_VariantMap& variantMap, const std::string& key, std::string&& value)
    {
        GB_VariantMap::iterator iter = variantMap.lower_bound(key);
        if (iter != variantMap.end() && iter->first == key)
        {
            iter->second = std::move(value);
            return;
        }

        variantMap.emplace_hint(iter, key, GB_Variant(std::move(value)));
    }

    std::string BuildXmlQualifiedName(const xmlChar* localName, const xmlChar* namespacePrefix)
    {
        const std::string localNameText = localName != nullptr
            ? reinterpret_cast<const char*>(localName)
            : std::string();
        const std::string namespacePrefixText = namespacePrefix != nullptr
            ? reinterpret_cast<const char*>(namespacePrefix)
            : std::string();

        if (namespacePrefixText.empty())
        {
            return localNameText;
        }

        if (localNameText.empty())
        {
            return namespacePrefixText;
        }

        return namespacePrefixText + ":" + localNameText;
    }

    bool ConvertJsonNodeToVariant(const CPLJSONObject& jsonObject, GB_Variant& outValue);

    bool ConvertJsonObjectToVariantMap(const CPLJSONObject& jsonObject, GB_VariantMap& outMap)
    {
        if (!jsonObject.IsValid() || jsonObject.GetType() != CPLJSONObject::Type::Object)
        {
            return false;
        }

        try
        {
            GB_VariantMap newMap;
            const std::vector<CPLJSONObject> children = jsonObject.GetChildren();
            for (std::size_t index = 0; index < children.size(); index++)
            {
                const CPLJSONObject& child = children[index];
                GB_Variant childValue;
                if (!ConvertJsonNodeToVariant(child, childValue))
                {
                    return false;
                }

                InsertOrAssignVariantMapValue(newMap, child.GetName(), std::move(childValue));
            }

            outMap = std::move(newMap);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ConvertJsonArrayToVariantList(const CPLJSONArray& jsonArray, GB_VariantList& outList)
    {
        try
        {
            GB_VariantList newList;
            const int itemCount = jsonArray.Size();
            if (itemCount > 0)
            {
                newList.reserve(static_cast<std::size_t>(itemCount));
            }

            for (int index = 0; index < itemCount; index++)
            {
                GB_Variant itemValue;
                if (!ConvertJsonNodeToVariant(jsonArray[index], itemValue))
                {
                    return false;
                }

                newList.push_back(std::move(itemValue));
            }

            outList = std::move(newList);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ConvertJsonNodeToVariant(const CPLJSONObject& jsonObject, GB_Variant& outValue)
    {
        if (!jsonObject.IsValid())
        {
            return false;
        }

        switch (jsonObject.GetType())
        {
        case CPLJSONObject::Type::Null:
            outValue.Reset();
            return true;

        case CPLJSONObject::Type::Boolean:
            outValue = jsonObject.ToBool();
            return true;

        case CPLJSONObject::Type::String:
            outValue = jsonObject.ToString();
            return true;

        case CPLJSONObject::Type::Integer:
            outValue = jsonObject.ToInteger();
            return true;

        case CPLJSONObject::Type::Long:
            outValue = static_cast<long long>(jsonObject.ToLong());
            return true;

        case CPLJSONObject::Type::Double:
            outValue = jsonObject.ToDouble();
            return true;

        case CPLJSONObject::Type::Object:
        {
            GB_VariantMap objectValue;
            if (!ConvertJsonObjectToVariantMap(jsonObject, objectValue))
            {
                return false;
            }

            outValue = std::move(objectValue);
            return true;
        }

        case CPLJSONObject::Type::Array:
        {
            GB_VariantList arrayValue;
            if (!ConvertJsonArrayToVariantList(jsonObject.ToArray(), arrayValue))
            {
                return false;
            }

            outValue = std::move(arrayValue);
            return true;
        }

        case CPLJSONObject::Type::Unknown:
        default:
            return false;
        }
    }


    bool LoadJsonDocumentFromText(const std::string& jsonText, CPLJSONDocument& outDocument, std::string* errorMessage)
    {
        ClearOptionalErrorMessage(errorMessage);

        if (jsonText.empty())
        {
            AssignOptionalErrorMessage(errorMessage, "Input JSON text is empty.");
            return false;
        }

        if (!outDocument.LoadMemory(jsonText))
        {
            AssignOptionalErrorMessage(errorMessage, "Failed to parse JSON text. Please ensure the input is valid JSON.");
            return false;
        }

        return true;
    }

    std::string XmlCharToString(const xmlChar* text)
    {
        if (text == nullptr)
        {
            return std::string();
        }

        return std::string(reinterpret_cast<const char*>(text));
    }

    std::string TrimAsciiWhitespace(const std::string& text)
    {
        std::size_t beginIndex = 0;
        while (beginIndex < text.size())
        {
            const char character = text[beginIndex];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
            {
                break;
            }

            beginIndex++;
        }

        std::size_t endIndex = text.size();
        while (endIndex > beginIndex)
        {
            const char character = text[endIndex - 1];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
            {
                break;
            }

            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    bool IsAsciiWhitespaceOnly(const std::string& text)
    {
        for (std::size_t index = 0; index < text.size(); index++)
        {
            const char character = text[index];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n')
            {
                return false;
            }
        }

        return true;
    }

    std::string NormalizeXmlText(const xmlNode* textNode, const std::string& rawText, const XmlOptions& options, bool& shouldKeep)
    {
        shouldKeep = true;

        std::string text = rawText;
        if (options.trimText)
        {
            text = TrimAsciiWhitespace(text);
            if (text.empty() && options.skipEmptyTextAfterTrim)
            {
                shouldKeep = false;
                return std::string();
            }
        }

        if (!options.preserveBlankText && IsAsciiWhitespaceOnly(text))
        {
            const int xmlSpace = xmlNodeGetSpacePreserve(textNode);
            if (xmlSpace != 1)
            {
                shouldKeep = false;
                return std::string();
            }
        }

        if (text.empty() && !options.preserveBlankText && options.skipEmptyTextAfterTrim)
        {
            shouldKeep = false;
            return std::string();
        }

        return text;
    }

    void TrimTrailingLineBreaks(std::string& text)
    {
        while (!text.empty())
        {
            const char lastCharacter = text[text.size() - 1];
            if (lastCharacter != '\r' && lastCharacter != '\n')
            {
                break;
            }

            text.erase(text.size() - 1);
        }
    }

    XmlDiagnostic BuildDiagnosticFromXmlError(const xmlError& error)
    {
        XmlDiagnostic diagnostic;
        diagnostic.level = static_cast<int>(error.level);
        diagnostic.code = error.code;
        diagnostic.line = error.line;
        diagnostic.column = error.int2;
        diagnostic.file = error.file != nullptr ? error.file : std::string();
        diagnostic.message = error.message != nullptr ? error.message : std::string();
        TrimTrailingLineBreaks(diagnostic.message);
        return diagnostic;
    }

    struct XmlErrorCollector
    {
        explicit XmlErrorCollector(std::size_t maxCountIn) : maxCount(maxCountIn)
        {
        }

#if LIBXML_VERSION >= 21200
        static void XMLCALL StructuredErrorCallback(void* userData, const xmlError* error)
#else
        static void XMLCALL StructuredErrorCallback(void* userData, xmlErrorPtr error)
#endif
        {
            if (userData == nullptr || error == nullptr)
            {
                return;
            }

            XmlErrorCollector* collector = static_cast<XmlErrorCollector*>(userData);
            collector->Add(*error);
        }

        void Add(const xmlError& error)
        {
            if (diagnostics.size() >= maxCount)
            {
                return;
            }

            diagnostics.push_back(BuildDiagnosticFromXmlError(error));
        }

        std::vector<XmlDiagnostic> diagnostics;
        std::size_t maxCount = 0;
    };

    std::string BuildXmlErrorMessage(const std::vector<XmlDiagnostic>& diagnostics, const std::string& fallbackMessage)
    {
        if (diagnostics.empty())
        {
            return fallbackMessage;
        }

        const XmlDiagnostic* selectedDiagnostic = &diagnostics[0];
        for (std::size_t index = 1; index < diagnostics.size(); index++)
        {
            const XmlDiagnostic& currentDiagnostic = diagnostics[index];
            if (currentDiagnostic.level > selectedDiagnostic->level)
            {
                selectedDiagnostic = &currentDiagnostic;
                continue;
            }

            if (currentDiagnostic.level == selectedDiagnostic->level && currentDiagnostic.code != 0 && selectedDiagnostic->code == 0)
            {
                selectedDiagnostic = &currentDiagnostic;
            }
        }

        std::ostringstream stream;
        stream << selectedDiagnostic->message;
        if (selectedDiagnostic->line > 0)
        {
            stream << " (line=" << selectedDiagnostic->line;
            if (selectedDiagnostic->column > 0)
            {
                stream << ", column=" << selectedDiagnostic->column;
            }
            stream << ")";
        }

        if (!selectedDiagnostic->file.empty())
        {
            stream << " [" << selectedDiagnostic->file << "]";
        }

        return stream.str();
    }

    int BuildXmlParserFlags(const XmlOptions& options, bool enableStructuredErrorHandler, bool& unsupportedLegacySecurityCombination)
    {
        unsupportedLegacySecurityCombination = false;

        int flags = 0;

        if (options.allowRecovery)
        {
            flags |= XML_PARSE_RECOVER;
        }

        if (!options.allowNetwork)
        {
            flags |= XML_PARSE_NONET;
        }

        if (options.expandEntities)
        {
            flags |= XML_PARSE_NOENT;
        }

        if (options.loadExternalDtd)
        {
            flags |= XML_PARSE_DTDLOAD;
        }

        if (options.applyDefaultDtdAttributes)
        {
            flags |= XML_PARSE_DTDATTR;
        }

        if (options.validateDtd)
        {
            flags |= XML_PARSE_DTDVALID;
        }

        if (options.pedantic)
        {
            flags |= XML_PARSE_PEDANTIC;
        }

        if (!options.preserveCDataSections)
        {
            flags |= XML_PARSE_NOCDATA;
        }

        if (options.cleanupRedundantNamespaces)
        {
            flags |= XML_PARSE_NSCLEAN;
        }

        if (options.compactMemory)
        {
            flags |= XML_PARSE_COMPACT;
        }

        if (options.useHugeMode)
        {
            flags |= XML_PARSE_HUGE;
        }

#ifdef XML_PARSE_BIG_LINES
        flags |= XML_PARSE_BIG_LINES;
#endif

#ifdef XML_PARSE_IGNORE_ENC
        if (options.ignoreEncodingDeclaration)
        {
            flags |= XML_PARSE_IGNORE_ENC;
        }
#endif

#ifdef XML_PARSE_NO_XXE
        if (!options.allowExternalEntities)
        {
            flags |= XML_PARSE_NO_XXE;
        }
#else
        if (!options.allowExternalEntities
            && (options.expandEntities || options.loadExternalDtd || options.applyDefaultDtdAttributes || options.validateDtd))
        {
            unsupportedLegacySecurityCombination = true;
        }
#endif

        if (!enableStructuredErrorHandler)
        {
            flags |= XML_PARSE_NOERROR;
            flags |= XML_PARSE_NOWARNING;
        }

        return flags;
    }

    std::string GetXmlNodeTypeName(const xmlNode* node)
    {
        if (node == nullptr)
        {
            return "unknown";
        }

        switch (node->type)
        {
        case XML_ELEMENT_NODE:
            return "element";

        case XML_TEXT_NODE:
            return "text";

        case XML_CDATA_SECTION_NODE:
            return "cdata";

        case XML_COMMENT_NODE:
            return "comment";

        case XML_PI_NODE:
            return "processingInstruction";

        case XML_ENTITY_REF_NODE:
            return "entityReference";

        case XML_ENTITY_NODE:
            return "entity";

        case XML_DOCUMENT_NODE:
            return "document";

        case XML_DOCUMENT_TYPE_NODE:
            return "documentType";

        case XML_DTD_NODE:
            return "dtd";

        case XML_ATTRIBUTE_NODE:
            return "attribute";

        case XML_NAMESPACE_DECL:
            return "namespaceDeclaration";

        case XML_DOCUMENT_FRAG_NODE:
            return "documentFragment";

        default:
            return "unknown";
        }
    }

    void AppendNodeIdentityFields(const xmlNode* node, const XmlOptions& options, GB_VariantMap& outNode)
    {
        if (node == nullptr)
        {
            return;
        }

        outNode["nodeType"] = GetXmlNodeTypeName(node);

        if (node->name != nullptr)
        {
            const std::string localName = XmlCharToString(node->name);
            const xmlChar* const namespacePrefix = node->ns != nullptr ? node->ns->prefix : nullptr;
            const std::string qualifiedName = BuildXmlQualifiedName(node->name, namespacePrefix);

            if (!qualifiedName.empty())
            {
                outNode["name"] = qualifiedName;
            }
            if (!localName.empty())
            {
                outNode["localName"] = localName;
            }
        }

        if (options.includeNamespaceInfo && node->ns != nullptr)
        {
            const std::string namespacePrefix = XmlCharToString(node->ns->prefix);
            const std::string namespaceUri = XmlCharToString(node->ns->href);
            if (!namespacePrefix.empty())
            {
                outNode["namespacePrefix"] = namespacePrefix;
            }
            if (!namespaceUri.empty())
            {
                outNode["namespaceUri"] = namespaceUri;
            }
        }

        if (options.includeNodeLineNumbers)
        {
            const long long lineNumber = static_cast<long long>(xmlGetLineNo(node));
            if (lineNumber > 0)
            {
                outNode["line"] = lineNumber;
            }
        }
    }

    bool BuildXmlAttributeValue(const xmlAttr* attribute, const xmlDoc* document, std::string& outValue)
    {
        if (attribute == nullptr)
        {
            return false;
        }

        UniqueXmlChar value(xmlNodeListGetString(const_cast<xmlDoc*>(document), attribute->children, 1));
        if (value.get() == nullptr)
        {
            outValue.clear();
            return true;
        }

        outValue = XmlCharToString(value.get());
        return true;
    }

    bool BuildXmlAttributes(const xmlNode* node, const xmlDoc* document, const XmlOptions& options, GB_VariantMap& outAttributes)
    {
        outAttributes.clear();

        if (node == nullptr || document == nullptr)
        {
            return false;
        }

        try
        {
            for (const xmlAttr* attribute = node->properties; attribute != nullptr; attribute = attribute->next)
            {
                if (attribute->name == nullptr)
                {
                    continue;
                }

                const xmlChar* const attributePrefix = attribute->ns != nullptr ? attribute->ns->prefix : nullptr;
                const std::string attributeName = BuildXmlQualifiedName(attribute->name, attributePrefix);

                std::string attributeValue;
                if (!BuildXmlAttributeValue(attribute, document, attributeValue))
                {
                    return false;
                }

                if (options.attributeOutputMode == GB_XmlParser::AttributeOutputMode::ValueOnly)
                {
                    InsertOrAssignVariantMapString(outAttributes, attributeName, std::move(attributeValue));
                    continue;
                }

                GB_VariantMap attributeObject;
                attributeObject["nodeType"] = "attribute";
                attributeObject["name"] = attributeName;
                attributeObject["localName"] = XmlCharToString(attribute->name);
                attributeObject["value"] = std::move(attributeValue);
                if (options.includeNamespaceInfo && attribute->ns != nullptr)
                {
                    const std::string attributePrefix = XmlCharToString(attribute->ns->prefix);
                    const std::string attributeUri = XmlCharToString(attribute->ns->href);
                    if (!attributePrefix.empty())
                    {
                        attributeObject["namespacePrefix"] = attributePrefix;
                    }
                    if (!attributeUri.empty())
                    {
                        attributeObject["namespaceUri"] = attributeUri;
                    }
                }

                InsertOrAssignVariantMapValue(outAttributes, attributeName, std::move(attributeObject));
            }
        }
        catch (...)
        {
            outAttributes.clear();
            return false;
        }

        return true;
    }

    bool BuildXmlNamespaceDeclarations(const xmlNode* node, GB_VariantMap& outNamespaces)
    {
        outNamespaces.clear();
        if (node == nullptr)
        {
            return false;
        }

        try
        {
            for (const xmlNs* namespaceNode = node->nsDef; namespaceNode != nullptr; namespaceNode = namespaceNode->next)
            {
                const std::string prefix = XmlCharToString(namespaceNode->prefix);
                std::string uri = XmlCharToString(namespaceNode->href);
                InsertOrAssignVariantMapString(outNamespaces, prefix, std::move(uri));
            }
        }
        catch (...)
        {
            outNamespaces.clear();
            return false;
        }

        return true;
    }

    bool ConvertXmlNodeToVariant(const xmlNode* node, const xmlDoc* document, const XmlOptions& options, GB_Variant& outValue);

    bool TryAppendMergedTextChild(GB_VariantList& outChildren, const std::string& nodeType, const std::string& text)
    {
        if (outChildren.empty())
        {
            return false;
        }

        GB_Variant& lastChild = outChildren.back();
        GB_VariantMap* lastChildMap = lastChild.AnyCast<GB_VariantMap>();
        if (lastChildMap == nullptr)
        {
            return false;
        }

        GB_VariantMap::iterator typeIter = lastChildMap->find("nodeType");
        if (typeIter == lastChildMap->end())
        {
            return false;
        }

        const std::string* typeText = typeIter->second.AnyCast<std::string>();
        if (typeText == nullptr || *typeText != nodeType)
        {
            return false;
        }

        GB_VariantMap::iterator textIter = lastChildMap->find("text");
        if (textIter == lastChildMap->end())
        {
            return false;
        }

        std::string* existingText = textIter->second.AnyCast<std::string>();
        if (existingText == nullptr)
        {
            return false;
        }

        existingText->append(text);
        return true;
    }

    bool ShouldSkipXmlChildNode(const xmlNode* childNode, const XmlOptions& options)
    {
        if (childNode == nullptr)
        {
            return true;
        }

        if (childNode->type == XML_COMMENT_NODE && !options.preserveComments)
        {
            return true;
        }

        if (childNode->type == XML_PI_NODE && !options.preserveProcessingInstructions)
        {
            return true;
        }

        if (childNode->type == XML_DTD_NODE)
        {
            return true;
        }

        return false;
    }

    bool BuildXmlChildren(
        const xmlNode* parentNode,
        const xmlDoc* document,
        const XmlOptions& options,
        GB_VariantList& outChildren,
        std::string& outDirectText)
    {
        outChildren.clear();
        outDirectText.clear();

        if (parentNode == nullptr || document == nullptr)
        {
            return false;
        }

        try
        {
            std::size_t childCount = 0;
            for (const xmlNode* childNode = parentNode->children; childNode != nullptr; childNode = childNode->next)
            {
                if (ShouldSkipXmlChildNode(childNode, options))
                {
                    continue;
                }

                childCount++;
            }

            if (childCount > 0)
            {
                outChildren.reserve(childCount);
            }

            for (const xmlNode* childNode = parentNode->children; childNode != nullptr; childNode = childNode->next)
            {
                if (ShouldSkipXmlChildNode(childNode, options))
                {
                    continue;
                }

                if (childNode->type == XML_TEXT_NODE || childNode->type == XML_CDATA_SECTION_NODE)
                {
                    bool shouldKeepText = false;
                    const std::string normalizedText = NormalizeXmlText(childNode, XmlCharToString(childNode->content), options, shouldKeepText);
                    if (!shouldKeepText)
                    {
                        continue;
                    }

                    outDirectText.append(normalizedText);

                    const std::string logicalNodeType = childNode->type == XML_CDATA_SECTION_NODE ? "cdata" : "text";
                    if (options.mergeAdjacentText && TryAppendMergedTextChild(outChildren, logicalNodeType, normalizedText))
                    {
                        continue;
                    }
                }

                GB_Variant childValue;
                if (!ConvertXmlNodeToVariant(childNode, document, options, childValue))
                {
                    return false;
                }

                outChildren.push_back(std::move(childValue));
            }
        }
        catch (...)
        {
            outChildren.clear();
            outDirectText.clear();
            return false;
        }

        return true;
    }

    bool ConvertXmlTextLikeNodeToVariant(const xmlNode* node, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr)
        {
            return false;
        }

        bool shouldKeepText = false;
        const std::string normalizedText = NormalizeXmlText(node, XmlCharToString(node->content), options, shouldKeepText);
        if (!shouldKeepText)
        {
            return false;
        }

        GB_VariantMap nodeObject;
        AppendNodeIdentityFields(node, options, nodeObject);
        nodeObject["text"] = normalizedText;
        outValue = std::move(nodeObject);
        return true;
    }

    bool ConvertXmlCommentNodeToVariant(const xmlNode* node, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr)
        {
            return false;
        }

        GB_VariantMap nodeObject;
        AppendNodeIdentityFields(node, options, nodeObject);
        nodeObject["text"] = XmlCharToString(node->content);
        outValue = std::move(nodeObject);
        return true;
    }

    bool ConvertXmlProcessingInstructionToVariant(const xmlNode* node, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr)
        {
            return false;
        }

        GB_VariantMap nodeObject;
        AppendNodeIdentityFields(node, options, nodeObject);
        nodeObject["text"] = XmlCharToString(node->content);
        outValue = std::move(nodeObject);
        return true;
    }

    bool ConvertXmlGenericNodeToVariant(const xmlNode* node, const xmlDoc* document, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr || document == nullptr)
        {
            return false;
        }

        GB_VariantMap nodeObject;
        AppendNodeIdentityFields(node, options, nodeObject);

        UniqueXmlChar content(xmlNodeGetContent(node));
        if (content.get() != nullptr)
        {
            nodeObject["text"] = XmlCharToString(content.get());
        }

        GB_VariantList children;
        std::string directText;
        if (!BuildXmlChildren(node, document, options, children, directText))
        {
            return false;
        }

        if (!children.empty())
        {
            nodeObject["children"] = std::move(children);
        }

        if (!directText.empty())
        {
            nodeObject["directText"] = std::move(directText);
        }

        outValue = std::move(nodeObject);
        return true;
    }

    bool ConvertXmlElementNodeToVariant(const xmlNode* node, const xmlDoc* document, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr || document == nullptr)
        {
            return false;
        }

        GB_VariantMap nodeObject;
        AppendNodeIdentityFields(node, options, nodeObject);

        GB_VariantMap attributes;
        if (!BuildXmlAttributes(node, document, options, attributes))
        {
            return false;
        }
        nodeObject["attributes"] = std::move(attributes);

        if (options.includeNamespaceDeclarations)
        {
            GB_VariantMap namespaceDeclarations;
            if (!BuildXmlNamespaceDeclarations(node, namespaceDeclarations))
            {
                return false;
            }
            nodeObject["namespaceDeclarations"] = std::move(namespaceDeclarations);
        }

        GB_VariantList children;
        std::string directText;
        if (!BuildXmlChildren(node, document, options, children, directText))
        {
            return false;
        }
        nodeObject["children"] = std::move(children);

        if (!directText.empty())
        {
            nodeObject["directText"] = std::move(directText);
        }

        outValue = std::move(nodeObject);
        return true;
    }

    bool ConvertXmlNodeToVariant(const xmlNode* node, const xmlDoc* document, const XmlOptions& options, GB_Variant& outValue)
    {
        if (node == nullptr || document == nullptr)
        {
            return false;
        }

        switch (node->type)
        {
        case XML_ELEMENT_NODE:
            return ConvertXmlElementNodeToVariant(node, document, options, outValue);

        case XML_TEXT_NODE:
            return ConvertXmlTextLikeNodeToVariant(node, options, outValue);

        case XML_CDATA_SECTION_NODE:
            return ConvertXmlTextLikeNodeToVariant(node, options, outValue);

        case XML_COMMENT_NODE:
            return ConvertXmlCommentNodeToVariant(node, options, outValue);

        case XML_PI_NODE:
            return ConvertXmlProcessingInstructionToVariant(node, options, outValue);

        default:
            return ConvertXmlGenericNodeToVariant(node, document, options, outValue);
        }
    }

    bool BuildXmlDoctypeObject(const xmlDoc* document, GB_VariantMap& outDoctype)
    {
        outDoctype.clear();
        if (document == nullptr)
        {
            return false;
        }

        try
        {
            const xmlDtd* internalSubset = document->intSubset;
            const xmlDtd* externalSubset = document->extSubset;
            const xmlDtd* doctype = internalSubset != nullptr ? internalSubset : externalSubset;
            if (doctype == nullptr)
            {
                return true;
            }

            outDoctype["nodeType"] = "documentType";
            if (doctype->name != nullptr)
            {
                outDoctype["name"] = XmlCharToString(doctype->name);
            }
            if (doctype->ExternalID != nullptr)
            {
                outDoctype["publicId"] = XmlCharToString(doctype->ExternalID);
            }
            if (doctype->SystemID != nullptr)
            {
                outDoctype["systemId"] = XmlCharToString(doctype->SystemID);
            }
        }
        catch (...)
        {
            outDoctype.clear();
            return false;
        }

        return true;
    }

    bool ConvertXmlDocumentToVariant(const xmlDoc* document, const XmlOptions& options, GB_Variant& outValue)
    {
        if (document == nullptr)
        {
            return false;
        }

        const xmlNode* rootElement = xmlDocGetRootElement(const_cast<xmlDoc*>(document));
        if (rootElement == nullptr)
        {
            return false;
        }

        if (options.outputMode == GB_XmlParser::OutputMode::RootElement)
        {
            return ConvertXmlNodeToVariant(rootElement, document, options, outValue);
        }

        try
        {
            GB_VariantMap documentObject;
            documentObject["nodeType"] = "document";

            if (options.includeDeclaration)
            {
                if (document->version != nullptr)
                {
                    documentObject["version"] = XmlCharToString(document->version);
                }
                if (document->encoding != nullptr)
                {
                    documentObject["encoding"] = XmlCharToString(document->encoding);
                }
                if (document->standalone == 0 || document->standalone == 1)
                {
                    documentObject["standalone"] = (document->standalone == 1);
                }
            }

            if (document->URL != nullptr)
            {
                documentObject["url"] = XmlCharToString(document->URL);
            }

            if (options.includeDoctype)
            {
                GB_VariantMap doctypeObject;
                if (!BuildXmlDoctypeObject(document, doctypeObject))
                {
                    return false;
                }
                if (!doctypeObject.empty())
                {
                    documentObject["doctype"] = std::move(doctypeObject);
                }
            }

            GB_VariantList children;
            std::size_t rootElementIndex = static_cast<std::size_t>(-1);

            std::size_t childCount = 0;
            for (const xmlNode* childNode = document->children; childNode != nullptr; childNode = childNode->next)
            {
                if (ShouldSkipXmlChildNode(childNode, options))
                {
                    continue;
                }

                childCount++;
            }
            if (childCount > 0)
            {
                children.reserve(childCount);
            }

            for (const xmlNode* childNode = document->children; childNode != nullptr; childNode = childNode->next)
            {
                if (ShouldSkipXmlChildNode(childNode, options))
                {
                    continue;
                }

                if (childNode == rootElement)
                {
                    rootElementIndex = children.size();
                }

                if (childNode->type == XML_TEXT_NODE || childNode->type == XML_CDATA_SECTION_NODE)
                {
                    bool shouldKeepText = false;
                    const std::string normalizedText = NormalizeXmlText(childNode, XmlCharToString(childNode->content), options, shouldKeepText);
                    if (!shouldKeepText)
                    {
                        continue;
                    }

                    const std::string logicalNodeType = childNode->type == XML_CDATA_SECTION_NODE ? "cdata" : "text";
                    if (options.mergeAdjacentText && TryAppendMergedTextChild(children, logicalNodeType, normalizedText))
                    {
                        continue;
                    }
                }

                GB_Variant childValue;
                if (!ConvertXmlNodeToVariant(childNode, document, options, childValue))
                {
                    return false;
                }

                children.push_back(std::move(childValue));
            }

            if (rootElementIndex == static_cast<std::size_t>(-1))
            {
                return false;
            }

            documentObject["children"] = std::move(children);
            documentObject["rootElementIndex"] = static_cast<unsigned long long>(rootElementIndex);
            outValue = std::move(documentObject);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseXmlTextInternal(
        const std::string& xmlText,
        GB_Variant& outValue,
        const XmlOptions& options,
        std::string* errorMessage,
        std::vector<XmlDiagnostic>* diagnostics)
    {
        ClearOptionalErrorMessage(errorMessage);
        ClearOptionalDiagnostics(diagnostics);

        if (xmlText.empty())
        {
            AssignOptionalErrorMessage(errorMessage, "Input XML text is empty.");
            return false;
        }

        if (xmlText.size() > static_cast<std::size_t>(INT_MAX))
        {
            AssignOptionalErrorMessage(errorMessage, "Input XML text is too large for libxml2 memory parsing.");
            return false;
        }

        EnsureXmlLibraryInitialized();

        XmlErrorCollector errorCollector(options.maxDiagnosticCount);

#if LIBXML_VERSION >= 21300
        const bool enableStructuredErrorHandler = true;
#else
        const bool enableStructuredErrorHandler = false;
#endif

        bool unsupportedLegacySecurityCombination = false;
        const int parserFlags = BuildXmlParserFlags(options, enableStructuredErrorHandler, unsupportedLegacySecurityCombination);
        if (unsupportedLegacySecurityCombination)
        {
            AssignOptionalErrorMessage(errorMessage, "The current libxml2 version does not provide XML_PARSE_NO_XXE, so enabling entity expansion or external DTD processing while disallowing external entities is unsupported.");
            return false;
        }

        UniqueXmlParserContext parserContext(xmlNewParserCtxt());
        if (!parserContext)
        {
            AssignOptionalErrorMessage(errorMessage, "Failed to create libxml2 parser context.");
            return false;
        }

#if LIBXML_VERSION >= 21300
        const xmlStructuredErrorFunc structuredErrorHandler = XmlErrorCollector::StructuredErrorCallback;
        xmlCtxtSetErrorHandler(parserContext.get(), structuredErrorHandler, &errorCollector);
#endif

        const char* const baseUrl = options.baseUrl.empty() ? nullptr : options.baseUrl.c_str();
        const char* const encodingHint = options.encodingHint.empty() ? nullptr : options.encodingHint.c_str();

        UniqueXmlDocument document(xmlCtxtReadMemory(
            parserContext.get(),
            xmlText.data(),
            static_cast<int>(xmlText.size()),
            baseUrl,
            encodingHint,
            parserFlags));

#if LIBXML_VERSION < 21300
        const xmlError* lastError = xmlCtxtGetLastError(parserContext.get());
        if (lastError != nullptr)
        {
            errorCollector.Add(*lastError);
        }
#endif

        if (diagnostics != nullptr)
        {
            *diagnostics = errorCollector.diagnostics;
        }

        if (!document)
        {
            AssignOptionalErrorMessage(errorMessage, BuildXmlErrorMessage(errorCollector.diagnostics, "Failed to parse XML text."));
            return false;
        }

        if (!options.allowRecovery && parserContext->wellFormed == 0)
        {
            AssignOptionalErrorMessage(errorMessage, BuildXmlErrorMessage(errorCollector.diagnostics, "XML text is not well-formed."));
            return false;
        }

        if (options.validateDtd && parserContext->valid == 0)
        {
            AssignOptionalErrorMessage(errorMessage, BuildXmlErrorMessage(errorCollector.diagnostics, "DTD validation failed."));
            return false;
        }

        GB_Variant parsedValue;
        if (!ConvertXmlDocumentToVariant(document.get(), options, parsedValue))
        {
            AssignOptionalErrorMessage(errorMessage, BuildXmlErrorMessage(errorCollector.diagnostics, "Failed to convert parsed XML tree to GB_Variant."));
            return false;
        }

        outValue = std::move(parsedValue);
        return true;
    }
}

bool GB_JsonParser::ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage)
{
    CPLJSONDocument jsonDocument;
    if (!LoadJsonDocumentFromText(jsonText, jsonDocument, errorMessage))
    {
        return false;
    }

    GB_Variant newValue;
    if (!ConvertJsonNodeToVariant(jsonDocument.GetRoot(), newValue))
    {
        AssignOptionalErrorMessage(errorMessage, "Failed to convert JSON structure to GB_Variant. The JSON may contain unsupported types or structures.");
        return false;
    }

    outValue = std::move(newValue);
    return true;
}

bool GB_JsonParser::ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage)
{
    CPLJSONDocument jsonDocument;
    if (!LoadJsonDocumentFromText(jsonText, jsonDocument, errorMessage))
    {
        return false;
    }

    const CPLJSONObject rootObject = jsonDocument.GetRoot();
    GB_VariantMap newMap;
    if (!ConvertJsonObjectToVariantMap(rootObject, newMap))
    {
        AssignOptionalErrorMessage(errorMessage, "Failed to convert JSON structure to GB_VariantMap. The JSON root must be an object, and it may contain unsupported types or structures.");
        return false;
    }

    outMap = std::move(newMap);
    return true;
}

bool GB_XmlParser::ParseToVariant(
    const std::string& xmlText,
    GB_Variant& outValue,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    return ParseToVariant(xmlText, outValue, Options(), errorMessage, diagnostics);
}

bool GB_XmlParser::ParseToVariant(
    const std::string& xmlText,
    GB_Variant& outValue,
    const Options& options,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    return ParseXmlTextInternal(xmlText, outValue, options, errorMessage, diagnostics);
}

bool GB_XmlParser::ParseToVariantMap(
    const std::string& xmlText,
    GB_VariantMap& outMap,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    return ParseToVariantMap(xmlText, outMap, Options(), errorMessage, diagnostics);
}

bool GB_XmlParser::ParseToVariantMap(
    const std::string& xmlText,
    GB_VariantMap& outMap,
    const Options& options,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    GB_Variant value;
    if (!ParseXmlTextInternal(xmlText, value, options, errorMessage, diagnostics))
    {
        return false;
    }

    const GB_VariantMap* valueMap = value.AnyCast<GB_VariantMap>();
    if (valueMap == nullptr)
    {
        AssignOptionalErrorMessage(errorMessage, "The parsed XML result is not a GB_VariantMap.");
        return false;
    }

    outMap = *valueMap;
    return true;
}

bool GB_XmlParser::ParseRootElementToVariant(
    const std::string& xmlText,
    GB_Variant& outValue,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    return ParseRootElementToVariant(xmlText, outValue, Options(), errorMessage, diagnostics);
}

bool GB_XmlParser::ParseRootElementToVariant(
    const std::string& xmlText,
    GB_Variant& outValue,
    const Options& options,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    Options rootOptions = options;
    rootOptions.outputMode = OutputMode::RootElement;
    return ParseXmlTextInternal(xmlText, outValue, rootOptions, errorMessage, diagnostics);
}

bool GB_XmlParser::ParseRootElementToVariantMap(
    const std::string& xmlText,
    GB_VariantMap& outMap,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    return ParseRootElementToVariantMap(xmlText, outMap, Options(), errorMessage, diagnostics);
}

bool GB_XmlParser::ParseRootElementToVariantMap(
    const std::string& xmlText,
    GB_VariantMap& outMap,
    const Options& options,
    std::string* errorMessage,
    std::vector<Diagnostic>* diagnostics)
{
    GB_Variant value;
    Options rootOptions = options;
    rootOptions.outputMode = OutputMode::RootElement;
    if (!ParseXmlTextInternal(xmlText, value, rootOptions, errorMessage, diagnostics))
    {
        return false;
    }

    const GB_VariantMap* valueMap = value.AnyCast<GB_VariantMap>();
    if (valueMap == nullptr)
    {
        AssignOptionalErrorMessage(errorMessage, "The parsed XML root element is not a GB_VariantMap.");
        return false;
    }

    outMap = *valueMap;
    return true;
}
