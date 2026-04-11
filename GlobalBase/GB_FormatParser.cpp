#include "GB_FormatParser.h"

#include "cpl_error.h"
#include "cpl_json.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>

#include <algorithm>
#include <climits>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr int kMaxJsonNestingDepth = 256;

    void SetErrorMessage(std::string* errorMessage, const std::string& message)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
    }

    void ClearErrorMessage(std::string* errorMessage)
    {
        if (errorMessage != nullptr)
        {
            errorMessage->clear();
        }
    }

    std::string GetJsonTypeName(const CPLJSONObject::Type type)
    {
        switch (type)
        {
        case CPLJSONObject::Type::Unknown:
            return "Unknown";
        case CPLJSONObject::Type::Null:
            return "Null";
        case CPLJSONObject::Type::Object:
            return "Object";
        case CPLJSONObject::Type::Array:
            return "Array";
        case CPLJSONObject::Type::Boolean:
            return "Boolean";
        case CPLJSONObject::Type::String:
            return "String";
        case CPLJSONObject::Type::Integer:
            return "Integer";
        case CPLJSONObject::Type::Long:
            return "Long";
        case CPLJSONObject::Type::Double:
            return "Double";
        default:
            return "Unsupported";
        }
    }

    std::string EscapeJsonPathText(const std::string& text)
    {
        std::string result;
        result.reserve(text.size());

        for (std::size_t index = 0; index < text.size(); index++)
        {
            const char currentChar = text[index];
            if (currentChar == '\\' || currentChar == '"')
            {
                result.push_back('\\');
            }
            result.push_back(currentChar);
        }

        return result;
    }

    std::string GetJsonObjectChildPath(const std::string& parentPath, const std::string& childName)
    {
        return parentPath + "[\"" + EscapeJsonPathText(childName) + "\"]";
    }

    std::string GetJsonArrayItemPath(const std::string& parentPath, const int index)
    {
        return parentPath + "[" + std::to_string(index) + "]";
    }

    bool ConvertJsonNodeToVariant(const CPLJSONObject& jsonObject, GB_Variant& outValue, std::string* errorMessage, const std::string& jsonPath, const int currentDepth);

    bool ConvertJsonObjectToVariantMap(const CPLJSONObject& jsonObject, GB_VariantMap& outMap, std::string* errorMessage, const std::string& jsonPath, const int currentDepth)
    {
        if (!jsonObject.IsValid())
        {
            SetErrorMessage(errorMessage, "Invalid JSON object at " + jsonPath + ".");
            return false;
        }

        if (jsonObject.GetType() != CPLJSONObject::Type::Object)
        {
            SetErrorMessage(errorMessage, "JSON node at " + jsonPath + " is not an object. Actual type: " + GetJsonTypeName(jsonObject.GetType()) + ".");
            return false;
        }

        if (currentDepth > kMaxJsonNestingDepth)
        {
            SetErrorMessage(errorMessage, "JSON nesting depth exceeds the supported limit (" + std::to_string(kMaxJsonNestingDepth) + ") at " + jsonPath + ".");
            return false;
        }

        try
        {
            GB_VariantMap newMap;
            const std::vector<CPLJSONObject> children = jsonObject.GetChildren();

            for (std::size_t index = 0; index < children.size(); index++)
            {
                const CPLJSONObject& child = children[index];
                const std::string childName = child.GetName();
                const std::string childPath = GetJsonObjectChildPath(jsonPath, childName);

                GB_Variant childValue;
                if (!ConvertJsonNodeToVariant(child, childValue, errorMessage, childPath, currentDepth + 1))
                {
                    return false;
                }

                const GB_VariantMap::iterator insertPosition = newMap.lower_bound(childName);
                if (insertPosition != newMap.end() && insertPosition->first == childName)
                {
                    insertPosition->second = std::move(childValue);
                }
                else
                {
                    newMap.emplace_hint(insertPosition, childName, std::move(childValue));
                }
            }

            outMap = std::move(newMap);
            return true;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert JSON object at " + jsonPath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert JSON object at " + jsonPath + " due to an unknown exception.");
            return false;
        }
    }

    bool ConvertJsonArrayToVariantList(const CPLJSONArray& jsonArray, GB_VariantList& outList, std::string* errorMessage, const std::string& jsonPath, const int currentDepth)
    {
        if (currentDepth > kMaxJsonNestingDepth)
        {
            SetErrorMessage(errorMessage, "JSON nesting depth exceeds the supported limit (" + std::to_string(kMaxJsonNestingDepth) + ") at " + jsonPath + ".");
            return false;
        }

        try
        {
            GB_VariantList newList;
            const int itemCount = jsonArray.Size();

            if (itemCount < 0)
            {
                SetErrorMessage(errorMessage, "Invalid JSON array size at " + jsonPath + ".");
                return false;
            }

            if (itemCount > 0)
            {
                newList.reserve(static_cast<std::size_t>(itemCount));
            }

            for (int index = 0; index < itemCount; index++)
            {
                GB_Variant itemValue;
                if (!ConvertJsonNodeToVariant(jsonArray[index], itemValue, errorMessage, GetJsonArrayItemPath(jsonPath, index), currentDepth + 1))
                {
                    return false;
                }

                newList.push_back(std::move(itemValue));
            }

            outList = std::move(newList);
            return true;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert JSON array at " + jsonPath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert JSON array at " + jsonPath + " due to an unknown exception.");
            return false;
        }
    }

    bool ConvertJsonNodeToVariant(const CPLJSONObject& jsonObject, GB_Variant& outValue, std::string* errorMessage, const std::string& jsonPath, const int currentDepth)
    {
        if (!jsonObject.IsValid())
        {
            SetErrorMessage(errorMessage, "Invalid JSON node at " + jsonPath + ".");
            return false;
        }

        if (currentDepth > kMaxJsonNestingDepth)
        {
            SetErrorMessage(errorMessage, "JSON nesting depth exceeds the supported limit (" + std::to_string(kMaxJsonNestingDepth) + ") at " + jsonPath + ".");
            return false;
        }

        const CPLJSONObject::Type jsonType = jsonObject.GetType();
        switch (jsonType)
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
            if (!ConvertJsonObjectToVariantMap(jsonObject, objectValue, errorMessage, jsonPath, currentDepth))
            {
                return false;
            }

            outValue = std::move(objectValue);
            return true;
        }

        case CPLJSONObject::Type::Array:
        {
            GB_VariantList arrayValue;
            if (!ConvertJsonArrayToVariantList(jsonObject.ToArray(), arrayValue, errorMessage, jsonPath, currentDepth))
            {
                return false;
            }

            outValue = std::move(arrayValue);
            return true;
        }

        case CPLJSONObject::Type::Unknown:
        default:
            SetErrorMessage(errorMessage, "Unsupported JSON node type at " + jsonPath + ": " + GetJsonTypeName(jsonType) + ".");
            return false;
        }
    }

    bool LoadJsonDocument(const std::string& jsonText, CPLJSONDocument& jsonDocument, std::string* errorMessage)
    {
        if (jsonText.empty())
        {
            SetErrorMessage(errorMessage, "Input JSON text is empty.");
            return false;
        }

        CPLErrorReset();

        if (!jsonDocument.LoadMemory(jsonText))
        {
            const char* gdalErrorText = CPLGetLastErrorMsg();
            if (gdalErrorText != nullptr && gdalErrorText[0] != '\0')
            {
                SetErrorMessage(errorMessage, std::string("Failed to parse JSON text. GDAL error: ") + gdalErrorText);
            }
            else
            {
                SetErrorMessage(errorMessage, "Failed to parse JSON text. Please ensure the input is valid JSON.");
            }
            return false;
        }

        return true;
    }

    void EnsureLibXmlInitialized()
    {
        static std::once_flag initOnce;
        std::call_once(initOnce, []()
            {
                xmlInitParser();
            });
    }

    std::string XmlCharToString(const xmlChar* text)
    {
        if (text == nullptr)
        {
            return std::string();
        }

        return std::string(reinterpret_cast<const char*>(text));
    }

    bool IsXmlWhitespaceOnlyText(const std::string& text)
    {
        for (std::size_t index = 0; index < text.size(); index++)
        {
            const char currentChar = text[index];
            if (currentChar != ' ' && currentChar != '\t' && currentChar != '\n' && currentChar != '\r')
            {
                return false;
            }
        }

        return true;
    }

    GB_XmlDiagnostic::Level ConvertXmlDiagnosticLevel(const xmlErrorLevel level)
    {
        switch (level)
        {
        case XML_ERR_WARNING:
            return GB_XmlDiagnostic::Level::Warning;
        case XML_ERR_FATAL:
            return GB_XmlDiagnostic::Level::Fatal;
        case XML_ERR_ERROR:
        default:
            return GB_XmlDiagnostic::Level::Error;
        }
    }

    std::string NormalizeXmlDiagnosticMessage(const std::string& message)
    {
        std::size_t endIndex = message.size();
        while (endIndex > 0)
        {
            const char currentChar = message[endIndex - 1];
            if (currentChar != '\r' && currentChar != '\n')
            {
                break;
            }
            endIndex--;
        }

        return message.substr(0, endIndex);
    }

    class XmlErrorCollector
    {
    public:
        void Add(const xmlError* error)
        {
            if (error == nullptr)
            {
                return;
            }

            GB_XmlDiagnostic diagnostic;
            diagnostic.level = ConvertXmlDiagnosticLevel(static_cast<xmlErrorLevel>(error->level));
            diagnostic.code = error->code;
            diagnostic.lineNumber = error->line > 0 ? static_cast<long long>(error->line) : -1;
            diagnostic.columnNumber = error->int2 > 0 ? error->int2 : -1;
            diagnostic.message = NormalizeXmlDiagnosticMessage(XmlCharToString(reinterpret_cast<const xmlChar*>(error->message)));
            diagnostics_.push_back(std::move(diagnostic));
        }

        const std::vector<GB_XmlDiagnostic>& GetDiagnostics() const
        {
            return diagnostics_;
        }

        bool HasErrorOrFatal() const
        {
            for (std::size_t index = 0; index < diagnostics_.size(); index++)
            {
                if (diagnostics_[index].level != GB_XmlDiagnostic::Level::Warning)
                {
                    return true;
                }
            }

            return false;
        }

        std::string BuildSummaryMessage() const
        {
            if (diagnostics_.empty())
            {
                return std::string();
            }

            const GB_XmlDiagnostic& firstDiagnostic = diagnostics_.front();
            std::ostringstream stream;
            stream << "Failed to parse XML";
            if (firstDiagnostic.lineNumber > 0)
            {
                stream << " at line " << firstDiagnostic.lineNumber;
                if (firstDiagnostic.columnNumber > 0)
                {
                    stream << ", column " << firstDiagnostic.columnNumber;
                }
            }
            stream << ": " << firstDiagnostic.message;

            if (diagnostics_.size() > 1)
            {
                stream << " (and " << (diagnostics_.size() - 1) << " more message(s)).";
            }
            else if (!firstDiagnostic.message.empty() && firstDiagnostic.message.back() != '.')
            {
                stream << ".";
            }

            return stream.str();
        }

#if defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300
        static void StructuredErrorCallback(void* userData, const xmlError* error)
#else
        static void StructuredErrorCallback(void* userData, xmlErrorPtr error)
#endif
        {
            XmlErrorCollector* errorCollector = static_cast<XmlErrorCollector*>(userData);
            if (errorCollector != nullptr)
            {
                errorCollector->Add(error);
            }
        }

    private:
        std::vector<GB_XmlDiagnostic> diagnostics_;
    };

    struct XmlParserContextDeleter
    {
        void operator()(xmlParserCtxtPtr parserContext) const
        {
            if (parserContext != nullptr)
            {
                xmlFreeParserCtxt(parserContext);
            }
        }
    };

    struct XmlDocumentDeleter
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
                xmlMemFree(text);
            }
        }
    };

    std::string GetXmlNodePath(const xmlNodePtr node)
    {
        if (node == nullptr)
        {
            return std::string();
        }

        if (node->type == XML_DOCUMENT_NODE)
        {
            return "/";
        }

        std::vector<std::string> pathParts;
        xmlNodePtr currentNode = node;
        while (currentNode != nullptr && currentNode->type != XML_DOCUMENT_NODE)
        {
            std::string currentPart;
            switch (currentNode->type)
            {
            case XML_ELEMENT_NODE:
                currentPart = XmlCharToString(currentNode->name);
                break;
            case XML_TEXT_NODE:
                currentPart = "text()";
                break;
            case XML_CDATA_SECTION_NODE:
                currentPart = "cdata()";
                break;
            case XML_COMMENT_NODE:
                currentPart = "comment()";
                break;
            case XML_PI_NODE:
                currentPart = "processing-instruction()";
                break;
            case XML_ENTITY_REF_NODE:
                currentPart = "entity-ref(" + XmlCharToString(currentNode->name) + ")";
                break;
            default:
                currentPart = "node()";
                break;
            }

            int siblingIndex = 1;
            for (xmlNodePtr sibling = currentNode->prev; sibling != nullptr; sibling = sibling->prev)
            {
                if (sibling->type == currentNode->type)
                {
                    const bool sameElementName = currentNode->type != XML_ELEMENT_NODE
                        || xmlStrEqual(sibling->name, currentNode->name);
                    if (sameElementName)
                    {
                        siblingIndex++;
                    }
                }
            }

            pathParts.push_back(currentPart + "[" + std::to_string(siblingIndex) + "]");
            currentNode = currentNode->parent;
        }

        std::reverse(pathParts.begin(), pathParts.end());

        std::string path = "/";
        for (std::size_t index = 0; index < pathParts.size(); index++)
        {
            if (index > 0)
            {
                path += "/";
            }
            path += pathParts[index];
        }

        return path;
    }

    bool ShouldRejectUnsafeEntityConfiguration(const GB_XmlParserOptions& options)
    {
#if defined(XML_PARSE_NO_XXE)
        (void)options;
        return false;
#else
        return !options.allowExternalEntities
            && (options.substituteEntities
                || options.loadExternalDtd
                || options.applyDefaultDtdAttributes
                || options.validateWithDtd);
#endif
    }

    int BuildLibXmlParseOptions(const GB_XmlParserOptions& options)
    {
        int parseOptions = 0;

        if (options.allowRecovery)
        {
            parseOptions |= XML_PARSE_RECOVER;
        }

        if (!options.preserveCDataSections)
        {
            parseOptions |= XML_PARSE_NOCDATA;
        }

        if (options.cleanRedundantNamespaceDeclarations)
        {
            parseOptions |= XML_PARSE_NSCLEAN;
        }

        if (options.substituteEntities)
        {
            parseOptions |= XML_PARSE_NOENT;
        }

        if (options.loadExternalDtd)
        {
            parseOptions |= XML_PARSE_DTDLOAD;
        }

        if (options.applyDefaultDtdAttributes)
        {
            parseOptions |= XML_PARSE_DTDATTR;
        }

        if (options.validateWithDtd)
        {
            parseOptions |= XML_PARSE_DTDVALID;
        }

        if (!options.allowNetworkAccess)
        {
            parseOptions |= XML_PARSE_NONET;
        }

        if (options.allowHugeDocuments)
        {
            parseOptions |= XML_PARSE_HUGE;
        }

#ifdef XML_PARSE_COMPACT
        if (options.compactMemory)
        {
            parseOptions |= XML_PARSE_COMPACT;
        }
#endif

#ifdef XML_PARSE_BIG_LINES
        if (options.reportLargeLineNumbers)
        {
            parseOptions |= XML_PARSE_BIG_LINES;
        }
#endif

#ifdef XML_PARSE_NO_XXE
        if (!options.allowExternalEntities)
        {
            parseOptions |= XML_PARSE_NO_XXE;
        }
#endif

        return parseOptions;
    }

    bool AddXmlAttribute(const xmlAttrPtr attribute, GB_XmlAttribute& outAttribute)
    {
        if (attribute == nullptr)
        {
            return false;
        }

        outAttribute.attributeName = XmlCharToString(attribute->name);
        outAttribute.localName = outAttribute.attributeName;
        outAttribute.namespacePrefix.clear();
        outAttribute.namespaceUri.clear();

        if (attribute->ns != nullptr)
        {
            outAttribute.namespacePrefix = XmlCharToString(attribute->ns->prefix);
            outAttribute.namespaceUri = XmlCharToString(attribute->ns->href);
            if (!outAttribute.namespacePrefix.empty())
            {
                outAttribute.attributeName = outAttribute.namespacePrefix + ":" + outAttribute.localName;
            }
        }

        std::unique_ptr<xmlChar, XmlCharDeleter> attributeValue(xmlNodeListGetString(attribute->doc, attribute->children, 1));
        outAttribute.attributeValue = XmlCharToString(attributeValue.get());
        return true;
    }

    void AddNamespaceDeclarations(const xmlNodePtr node, std::vector<GB_XmlNamespaceDeclaration>& outNamespaceDeclarations)
    {
        if (node == nullptr)
        {
            return;
        }

        for (xmlNsPtr currentNamespace = node->nsDef; currentNamespace != nullptr; currentNamespace = currentNamespace->next)
        {
            GB_XmlNamespaceDeclaration namespaceDeclaration;
            namespaceDeclaration.namespacePrefix = XmlCharToString(currentNamespace->prefix);
            namespaceDeclaration.namespaceUri = XmlCharToString(currentNamespace->href);
            outNamespaceDeclarations.push_back(std::move(namespaceDeclaration));
        }
    }

    bool ConvertXmlNode(const xmlNodePtr node,
        GB_XmlNode& outNode,
        const GB_XmlParserOptions& options,
        std::string* errorMessage)
    {
        if (node == nullptr)
        {
            SetErrorMessage(errorMessage, "Cannot convert a null XML node.");
            return false;
        }

        try
        {
            GB_XmlNode newNode;
            newNode.lineNumber = static_cast<long long>(xmlGetLineNo(node));

            switch (node->type)
            {
            case XML_ELEMENT_NODE:
                newNode.nodeType = GB_XmlNode::Type::Element;
                newNode.localName = XmlCharToString(node->name);
                newNode.nodeTag = newNode.localName;
                if (node->ns != nullptr)
                {
                    newNode.namespacePrefix = XmlCharToString(node->ns->prefix);
                    newNode.namespaceUri = XmlCharToString(node->ns->href);
                    if (!newNode.namespacePrefix.empty())
                    {
                        newNode.nodeTag = newNode.namespacePrefix + ":" + newNode.localName;
                    }
                }

                for (xmlAttrPtr attribute = node->properties; attribute != nullptr; attribute = attribute->next)
                {
                    GB_XmlAttribute newAttribute;
                    if (!AddXmlAttribute(attribute, newAttribute))
                    {
                        SetErrorMessage(errorMessage, "Failed to read an attribute at " + GetXmlNodePath(node) + ".");
                        return false;
                    }
                    newNode.attributes.push_back(std::move(newAttribute));
                }

                if (options.includeNamespaceDeclarations)
                {
                    AddNamespaceDeclarations(node, newNode.namespaceDeclarations);
                }

                for (xmlNodePtr child = node->children; child != nullptr; child = child->next)
                {
                    if (child->type == XML_TEXT_NODE)
                    {
                        const std::string childText = XmlCharToString(child->content);
                        if (!options.preserveWhitespaceOnlyTextNodes && IsXmlWhitespaceOnlyText(childText))
                        {
                            continue;
                        }
                    }
                    else if (child->type == XML_CDATA_SECTION_NODE)
                    {
                        const std::string childText = XmlCharToString(child->content);
                        if (!options.preserveWhitespaceOnlyTextNodes && IsXmlWhitespaceOnlyText(childText))
                        {
                            continue;
                        }
                    }
                    else if (child->type == XML_COMMENT_NODE && !options.preserveComments)
                    {
                        continue;
                    }
                    else if (child->type == XML_PI_NODE && !options.preserveProcessingInstructions)
                    {
                        continue;
                    }
                    else if (child->type == XML_ENTITY_REF_NODE && !options.preserveEntityReferences)
                    {
                        continue;
                    }

                    GB_XmlNode childNode;
                    if (!ConvertXmlNode(child, childNode, options, errorMessage))
                    {
                        return false;
                    }
                    newNode.children.push_back(std::move(childNode));
                }
                break;

            case XML_TEXT_NODE:
                newNode.nodeType = GB_XmlNode::Type::Text;
                newNode.nodeValue = XmlCharToString(node->content);
                break;

            case XML_CDATA_SECTION_NODE:
                newNode.nodeType = GB_XmlNode::Type::CData;
                newNode.nodeValue = XmlCharToString(node->content);
                break;

            case XML_COMMENT_NODE:
                newNode.nodeType = GB_XmlNode::Type::Comment;
                newNode.nodeValue = XmlCharToString(node->content);
                break;

            case XML_PI_NODE:
                newNode.nodeType = GB_XmlNode::Type::ProcessingInstruction;
                newNode.nodeTag = XmlCharToString(node->name);
                newNode.localName = newNode.nodeTag;
                newNode.nodeValue = XmlCharToString(node->content);
                break;

            case XML_ENTITY_REF_NODE:
                newNode.nodeType = GB_XmlNode::Type::EntityReference;
                newNode.nodeTag = XmlCharToString(node->name);
                newNode.localName = newNode.nodeTag;
                for (xmlNodePtr child = node->children; child != nullptr; child = child->next)
                {
                    GB_XmlNode childNode;
                    if (!ConvertXmlNode(child, childNode, options, errorMessage))
                    {
                        return false;
                    }
                    newNode.children.push_back(std::move(childNode));
                }
                break;

            default:
                SetErrorMessage(errorMessage,
                    "Unsupported XML node type at " + GetXmlNodePath(node) + ". libxml2 node type: " + std::to_string(static_cast<int>(node->type)) + ".");
                return false;
            }

            outNode = std::move(newNode);
            return true;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert XML node at " + GetXmlNodePath(node) + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert XML node at " + GetXmlNodePath(node) + " due to an unknown exception.");
            return false;
        }
    }

    void LoadDocumentTypeInfo(const xmlDocPtr xmlDocument, GB_XmlDocument& outDocument)
    {
        if (xmlDocument == nullptr)
        {
            return;
        }

        outDocument.version = XmlCharToString(xmlDocument->version);
        outDocument.encoding = XmlCharToString(xmlDocument->encoding);
        outDocument.standalone = static_cast<GB_XmlDocument::StandaloneMode>(xmlDocument->standalone);

        if (xmlDocument->intSubset != nullptr)
        {
            outDocument.hasInternalSubset = true;
            outDocument.documentTypeName = XmlCharToString(xmlDocument->intSubset->name);
            outDocument.documentTypePublicId = XmlCharToString(xmlDocument->intSubset->ExternalID);
            outDocument.documentTypeSystemId = XmlCharToString(xmlDocument->intSubset->SystemID);
        }

        if (xmlDocument->extSubset != nullptr)
        {
            outDocument.hasExternalSubset = true;
            if (outDocument.documentTypeName.empty())
            {
                outDocument.documentTypeName = XmlCharToString(xmlDocument->extSubset->name);
            }
            if (outDocument.documentTypePublicId.empty())
            {
                outDocument.documentTypePublicId = XmlCharToString(xmlDocument->extSubset->ExternalID);
            }
            if (outDocument.documentTypeSystemId.empty())
            {
                outDocument.documentTypeSystemId = XmlCharToString(xmlDocument->extSubset->SystemID);
            }
        }
    }

    bool ConvertXmlDocument(const xmlDocPtr xmlDocument,
        GB_XmlDocument& outDocument,
        const GB_XmlParserOptions& options,
        const XmlErrorCollector& errorCollector,
        std::string* errorMessage)
    {
        if (xmlDocument == nullptr)
        {
            SetErrorMessage(errorMessage, "Parsed XML document is null.");
            return false;
        }

        try
        {
            GB_XmlDocument newDocument;
            LoadDocumentTypeInfo(xmlDocument, newDocument);
            newDocument.diagnostics = errorCollector.GetDiagnostics();
            newDocument.recovered = errorCollector.HasErrorOrFatal();

            xmlNodePtr documentElementNode = xmlDocGetRootElement(xmlDocument);
            if (documentElementNode == nullptr)
            {
                SetErrorMessage(errorMessage, "Parsed XML document does not contain a root element.");
                return false;
            }

            int topLevelElementCount = 0;
            bool rootElementSeen = false;

            for (xmlNodePtr child = xmlDocument->children; child != nullptr; child = child->next)
            {
                if (child->type == XML_DTD_NODE)
                {
                    continue;
                }

                if (child->type == XML_ELEMENT_NODE)
                {
                    topLevelElementCount++;
                    if (topLevelElementCount > 1)
                    {
                        SetErrorMessage(errorMessage, "Parsed XML document contains multiple top-level element nodes, which cannot be represented by GB_XmlDocument.");
                        return false;
                    }

                    if (!ConvertXmlNode(child, newDocument.rootNode, options, errorMessage))
                    {
                        return false;
                    }
                    rootElementSeen = true;
                    continue;
                }

                if (child->type == XML_COMMENT_NODE && !options.preserveComments)
                {
                    continue;
                }

                if (child->type == XML_PI_NODE && !options.preserveProcessingInstructions)
                {
                    continue;
                }

                GB_XmlNode miscNode;
                if (!ConvertXmlNode(child, miscNode, options, errorMessage))
                {
                    return false;
                }

                if (!rootElementSeen)
                {
                    newDocument.prologNodes.push_back(std::move(miscNode));
                }
                else
                {
                    newDocument.epilogNodes.push_back(std::move(miscNode));
                }
            }

            if (topLevelElementCount != 1)
            {
                SetErrorMessage(errorMessage, "Parsed XML document does not contain exactly one top-level element node.");
                return false;
            }

            outDocument = std::move(newDocument);
            return true;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, std::string("Failed to convert parsed XML document: ") + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert parsed XML document due to an unknown exception.");
            return false;
        }
    }

    bool ParseXmlDocumentInternal(const std::string& xmlText,
        GB_XmlDocument& outDocument,
        const GB_XmlParserOptions& options,
        std::string* errorMessage)
    {
        if (xmlText.empty())
        {
            SetErrorMessage(errorMessage, "Input XML text is empty.");
            return false;
        }

        if (xmlText.size() > static_cast<std::size_t>(INT_MAX))
        {
            SetErrorMessage(errorMessage, "Input XML text is too large for libxml2 memory parsing.");
            return false;
        }

        if (ShouldRejectUnsafeEntityConfiguration(options))
        {
            SetErrorMessage(errorMessage,
                "The requested XML parser options require external DTD/entity loading, but the current libxml2 version does not provide XML_PARSE_NO_XXE. Please either enable allowExternalEntities or disable substituteEntities / loadExternalDtd / applyDefaultDtdAttributes / validateWithDtd.");
            return false;
        }

        EnsureLibXmlInitialized();

        XmlErrorCollector errorCollector;
        std::unique_ptr<xmlParserCtxt, XmlParserContextDeleter> parserContext(xmlNewParserCtxt());
        if (!parserContext)
        {
            SetErrorMessage(errorMessage, "Failed to create libxml2 parser context.");
            return false;
        }

#if defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300
        xmlCtxtSetErrorHandler(parserContext.get(), XmlErrorCollector::StructuredErrorCallback, &errorCollector);
#else
        const xmlStructuredErrorFunc previousStructuredHandler = xmlStructuredError;
        void* const previousStructuredContext = xmlStructuredErrorContext;
        xmlSetStructuredErrorFunc(&errorCollector, XmlErrorCollector::StructuredErrorCallback);
#endif

        const int parseOptions = BuildLibXmlParseOptions(options);
        const char* const baseUrlText = options.baseUrl.empty() ? nullptr : options.baseUrl.c_str();
        const char* const forcedEncodingText = options.forcedEncoding.empty() ? nullptr : options.forcedEncoding.c_str();

        std::unique_ptr<xmlDoc, XmlDocumentDeleter> xmlDocument(
            xmlCtxtReadMemory(parserContext.get(),
                xmlText.data(),
                static_cast<int>(xmlText.size()),
                baseUrlText,
                forcedEncodingText,
                parseOptions));

#if !(defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300)
        xmlSetStructuredErrorFunc(previousStructuredContext, previousStructuredHandler);
#endif

        if (!xmlDocument)
        {
            const std::string summaryMessage = errorCollector.BuildSummaryMessage();
            if (!summaryMessage.empty())
            {
                SetErrorMessage(errorMessage, summaryMessage);
            }
            else
            {
                SetErrorMessage(errorMessage, "Failed to parse XML text.");
            }
            return false;
        }

        if (!ConvertXmlDocument(xmlDocument.get(), outDocument, options, errorCollector, errorMessage))
        {
            return false;
        }

        ClearErrorMessage(errorMessage);
        return true;
    }
}

bool GB_JsonParser::ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage)
{
    CPLJSONDocument jsonDocument;
    if (!LoadJsonDocument(jsonText, jsonDocument, errorMessage))
    {
        return false;
    }

    const CPLJSONObject rootObject = jsonDocument.GetRoot();
    if (!rootObject.IsValid())
    {
        SetErrorMessage(errorMessage, "Parsed JSON root node is invalid.");
        return false;
    }

    GB_Variant newValue;
    if (!ConvertJsonNodeToVariant(rootObject, newValue, errorMessage, "$", 0))
    {
        return false;
    }

    outValue = std::move(newValue);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_JsonParser::ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage)
{
    CPLJSONDocument jsonDocument;
    if (!LoadJsonDocument(jsonText, jsonDocument, errorMessage))
    {
        return false;
    }

    const CPLJSONObject rootObject = jsonDocument.GetRoot();
    if (!rootObject.IsValid())
    {
        SetErrorMessage(errorMessage, "Parsed JSON root node is invalid.");
        return false;
    }

    GB_VariantMap newMap;
    if (!ConvertJsonObjectToVariantMap(rootObject, newMap, errorMessage, "$", 0))
    {
        return false;
    }

    outMap = std::move(newMap);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_XmlParser::ParseToDocument(const std::string& xmlText,
    GB_XmlDocument& outDocument,
    const GB_XmlParserOptions& options,
    std::string* errorMessage)
{
    GB_XmlDocument newDocument;
    if (!ParseXmlDocumentInternal(xmlText, newDocument, options, errorMessage))
    {
        return false;
    }

    outDocument = std::move(newDocument);
    return true;
}

bool GB_XmlParser::ParseToRootNode(const std::string& xmlText,
    GB_XmlNode& outRootNode,
    const GB_XmlParserOptions& options,
    std::string* errorMessage)
{
    GB_XmlDocument document;
    if (!ParseXmlDocumentInternal(xmlText, document, options, errorMessage))
    {
        return false;
    }

    outRootNode = std::move(document.rootNode);
    ClearErrorMessage(errorMessage);
    return true;
}
