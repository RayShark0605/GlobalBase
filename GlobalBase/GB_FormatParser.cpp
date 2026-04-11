#include "GB_FormatParser.h"

#include "cpl_error.h"
#include "cpl_json.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlmemory.h>

#include <algorithm>
#include <climits>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#include <string>
#include <vector>

namespace
{
    constexpr int kMaxJsonNestingDepth = 256;
    constexpr std::size_t kUtf8BomLength = 3;

    bool HasUtf8Bom(const std::string& text)
    {
        return text.size() >= kUtf8BomLength
            && static_cast<unsigned char>(text[0]) == 0xEF
            && static_cast<unsigned char>(text[1]) == 0xBB
            && static_cast<unsigned char>(text[2]) == 0xBF;
    }

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

        const std::string* jsonTextToLoad = &jsonText;
        std::string jsonTextWithoutBom;
        if (HasUtf8Bom(jsonText))
        {
            jsonTextWithoutBom.assign(jsonText.begin() + static_cast<std::ptrdiff_t>(kUtf8BomLength), jsonText.end());
            jsonTextToLoad = &jsonTextWithoutBom;
        }

        if (jsonTextToLoad->empty())
        {
            SetErrorMessage(errorMessage, "Input JSON text is empty.");
            return false;
        }

        CPLErrorReset();

        if (!jsonDocument.LoadMemory(*jsonTextToLoad))
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

    bool IsXmlWhitespaceOnlyNode(const xmlNodePtr node)
    {
        if (node == nullptr)
        {
            return false;
        }

        if (node->type == XML_TEXT_NODE)
        {
            return xmlIsBlankNode(node) != 0;
        }

        if (node->type == XML_CDATA_SECTION_NODE)
        {
            return IsXmlWhitespaceOnlyText(XmlCharToString(node->content));
        }

        return false;
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
            if (error == nullptr || collectionFailed_)
            {
                return;
            }

            try
            {
                GB_XmlDiagnostic diagnostic;
                diagnostic.level = ConvertXmlDiagnosticLevel(static_cast<xmlErrorLevel>(error->level));
                diagnostic.code = error->code;
                diagnostic.lineNumber = error->line > 0 ? static_cast<long long>(error->line) : -1;
                diagnostic.columnNumber = error->int2 > 0 ? error->int2 : -1;
                diagnostic.message = NormalizeXmlDiagnosticMessage(XmlCharToString(reinterpret_cast<const xmlChar*>(error->message)));
                diagnostics_.push_back(std::move(diagnostic));
            }
            catch (...)
            {
                collectionFailed_ = true;
            }
        }

        const std::vector<GB_XmlDiagnostic>& GetDiagnostics() const
        {
            return diagnostics_;
        }

        std::string BuildSummaryMessage() const
        {
            if (diagnostics_.empty())
            {
                if (collectionFailed_)
                {
                    return "Failed to parse XML. Detailed diagnostics could not be collected.";
                }

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
                stream << " (and " << (diagnostics_.size() - 1) << " more message(s))";
            }
            else if (firstDiagnostic.message.empty() || firstDiagnostic.message.back() != '.')
            {
                stream << ".";
            }

            if (collectionFailed_)
            {
                stream << " Additional diagnostics could not be collected.";
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
        bool collectionFailed_ = false;
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

    xmlFreeFunc GetLibXmlFreeFunc()
    {
        static std::once_flag initOnce;
        static xmlFreeFunc freeFunc = nullptr;

        std::call_once(initOnce,
            []()
            {
                xmlMallocFunc mallocFunc = nullptr;
                xmlReallocFunc reallocFunc = nullptr;
                xmlStrdupFunc strdupFunc = nullptr;

                if (xmlMemGet(&freeFunc, &mallocFunc, &reallocFunc, &strdupFunc) != 0)
                {
                    freeFunc = reinterpret_cast<xmlFreeFunc>(free);
                }
            });

        return freeFunc;
    }

    struct XmlCharDeleter
    {
        void operator()(xmlChar* text) const
        {
            if (text == nullptr)
            {
                return;
            }

            const xmlFreeFunc freeFunc = GetLibXmlFreeFunc();
            if (freeFunc != nullptr)
            {
                freeFunc(text);
            }
        }
    };

    std::mutex& GetXmlStructuredErrorHandlerMutex()
    {
        static std::mutex structuredErrorHandlerMutex;
        return structuredErrorHandlerMutex;
    }

    class XmlStructuredErrorHandlerScope
    {
    public:
        XmlStructuredErrorHandlerScope(xmlParserCtxtPtr parserContext, XmlErrorCollector* errorCollector)
        {
#if defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300
            xmlCtxtSetErrorHandler(parserContext, XmlErrorCollector::StructuredErrorCallback, errorCollector);
#else
            (void)parserContext;
            previousStructuredHandler_ = xmlStructuredError;
            previousStructuredContext_ = xmlStructuredErrorContext;
            xmlSetStructuredErrorFunc(errorCollector, XmlErrorCollector::StructuredErrorCallback);
#endif
        }

        ~XmlStructuredErrorHandlerScope()
        {
#if !(defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300)
            xmlSetStructuredErrorFunc(previousStructuredContext_, previousStructuredHandler_);
#endif
        }

    private:
#if !(defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300)
        xmlStructuredErrorFunc previousStructuredHandler_ = nullptr;
        void* previousStructuredContext_ = nullptr;
#endif
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

    std::size_t GetXmlAttributeCount(const xmlNodePtr node)
    {
        std::size_t attributeCount = 0;
        for (xmlAttrPtr attribute = node != nullptr ? node->properties : nullptr; attribute != nullptr; attribute = attribute->next)
        {
            attributeCount++;
        }

        return attributeCount;
    }

    std::size_t GetXmlNamespaceDeclarationCount(const xmlNodePtr node)
    {
        std::size_t namespaceDeclarationCount = 0;
        for (xmlNsPtr currentNamespace = node != nullptr ? node->nsDef : nullptr; currentNamespace != nullptr; currentNamespace = currentNamespace->next)
        {
            namespaceDeclarationCount++;
        }

        return namespaceDeclarationCount;
    }

    xmlDocPtr GetXmlAttributeOwnerDocument(const xmlAttrPtr attribute)
    {
        if (attribute == nullptr)
        {
            return nullptr;
        }

        if (attribute->doc != nullptr)
        {
            return attribute->doc;
        }

        if (attribute->parent != nullptr)
        {
            return attribute->parent->doc;
        }

        return nullptr;
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

        if (attribute->children == nullptr)
        {
            outAttribute.attributeValue.clear();
            return true;
        }

        const xmlDocPtr attributeDocument = GetXmlAttributeOwnerDocument(attribute);
        std::unique_ptr<xmlChar, XmlCharDeleter> attributeValue(xmlNodeListGetString(attributeDocument, attribute->children, 1));
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

    bool ShouldSkipXmlChildNode(const xmlNodePtr child, const GB_XmlParserOptions& options)
    {
        if (child == nullptr)
        {
            return true;
        }

        if ((child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE)
            && !options.preserveWhitespaceOnlyTextNodes
            && IsXmlWhitespaceOnlyNode(child))
        {
            return true;
        }

        if (child->type == XML_COMMENT_NODE && !options.preserveComments)
        {
            return true;
        }

        if (child->type == XML_PI_NODE && !options.preserveProcessingInstructions)
        {
            return true;
        }

        if (child->type == XML_ENTITY_REF_NODE && !options.preserveEntityReferences)
        {
            return true;
        }

        return false;
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
            {
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

                const std::size_t attributeCount = GetXmlAttributeCount(node);
                if (attributeCount > 0)
                {
                    newNode.attributes.reserve(attributeCount);
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
                    const std::size_t namespaceDeclarationCount = GetXmlNamespaceDeclarationCount(node);
                    if (namespaceDeclarationCount > 0)
                    {
                        newNode.namespaceDeclarations.reserve(namespaceDeclarationCount);
                    }

                    AddNamespaceDeclarations(node, newNode.namespaceDeclarations);
                }

                for (xmlNodePtr child = node->children; child != nullptr; child = child->next)
                {
                    if (ShouldSkipXmlChildNode(child, options))
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
            }

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
                    if (ShouldSkipXmlChildNode(child, options))
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
        const bool recovered,
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
            newDocument.recovered = recovered;

            if (xmlDocGetRootElement(xmlDocument) == nullptr)
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

                if (ShouldSkipXmlChildNode(child, options))
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

    bool DidXmlParseRecover(const xmlParserCtxtPtr parserContext, const GB_XmlParserOptions& options)
    {
        if (parserContext == nullptr || !options.allowRecovery)
        {
            return false;
        }

        return parserContext->wellFormed == 0;
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

#if !(defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300)
        std::unique_lock<std::mutex> structuredErrorHandlerLock(GetXmlStructuredErrorHandlerMutex());
#endif

        XmlErrorCollector errorCollector;
        std::unique_ptr<xmlParserCtxt, XmlParserContextDeleter> parserContext(xmlNewParserCtxt());
        if (!parserContext)
        {
            SetErrorMessage(errorMessage, "Failed to create libxml2 parser context.");
            return false;
        }

        const XmlStructuredErrorHandlerScope structuredErrorHandlerScope(parserContext.get(), &errorCollector);

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

        const bool recovered = DidXmlParseRecover(parserContext.get(), options);
        if (!ConvertXmlDocument(xmlDocument.get(), outDocument, options, errorCollector, recovered, errorMessage))
        {
            return false;
        }

        ClearErrorMessage(errorMessage);
        return true;
    }
}

namespace
{
    char FoldAsciiCase(const char character)
    {
        if (character >= 'A' && character <= 'Z')
        {
            return static_cast<char>(character - 'A' + 'a');
        }

        return character;
    }

    bool EqualsAsciiText(const std::string& leftText, const std::string& rightText, const bool caseSensitive)
    {
        if (leftText.size() != rightText.size())
        {
            return false;
        }

        if (caseSensitive)
        {
            return leftText == rightText;
        }

        for (std::size_t index = 0; index < leftText.size(); index++)
        {
            if (FoldAsciiCase(leftText[index]) != FoldAsciiCase(rightText[index]))
            {
                return false;
            }
        }

        return true;
    }

    bool IsXmlNameMatched(const std::string& targetName, const std::string& fullName, const std::string& localNodeName, const bool caseSensitive)
    {
        if (targetName.empty())
        {
            return false;
        }

        return EqualsAsciiText(targetName, fullName, caseSensitive)
            || (!localNodeName.empty() && EqualsAsciiText(targetName, localNodeName, caseSensitive));
    }

    void AppendXmlTextValue(const GB_XmlNode& node, std::string& outValue)
    {
        switch (node.nodeType)
        {
        case GB_XmlNode::Type::Text:
        case GB_XmlNode::Type::CData:
            outValue += node.nodeValue;
            break;

        case GB_XmlNode::Type::Element:
        case GB_XmlNode::Type::EntityReference:
            for (std::size_t index = 0; index < node.children.size(); index++)
            {
                AppendXmlTextValue(node.children[index], outValue);
            }
            break;

        case GB_XmlNode::Type::Comment:
        case GB_XmlNode::Type::ProcessingInstruction:
        default:
            break;
        }
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

bool GB_XmlNode::IsElement(const std::string& elementName, bool caseSensitive) const
{
    if (nodeType != Type::Element)
    {
        return false;
    }

    return IsXmlNameMatched(elementName, nodeTag, localName, caseSensitive);
}

bool GB_XmlNode::HasAttribute(const std::string& attributeName, bool caseSensitive) const
{
    return GetAttribute(attributeName, caseSensitive) != nullptr;
}

const GB_XmlAttribute* GB_XmlNode::GetAttribute(const std::string& attributeName, bool caseSensitive) const
{
    if (attributeName.empty())
    {
        return nullptr;
    }

    for (std::size_t index = 0; index < attributes.size(); index++)
    {
        const GB_XmlAttribute& attribute = attributes[index];
        if (IsXmlNameMatched(attributeName, attribute.attributeName, attribute.localName, caseSensitive))
        {
            return &attribute;
        }
    }

    return nullptr;
}

GB_XmlAttribute* GB_XmlNode::GetAttribute(const std::string& attributeName, bool caseSensitive)
{
    return const_cast<GB_XmlAttribute*>(static_cast<const GB_XmlNode&>(*this).GetAttribute(attributeName, caseSensitive));
}

bool GB_XmlNode::TryGetAttributeValue(const std::string& attributeName, std::string& outValue, bool caseSensitive) const
{
    const GB_XmlAttribute* attribute = GetAttribute(attributeName, caseSensitive);
    if (attribute == nullptr)
    {
        return false;
    }

    outValue = attribute->attributeValue;
    return true;
}

std::string GB_XmlNode::GetAttributeValue(const std::string& attributeName, bool caseSensitive) const
{
    const GB_XmlAttribute* attribute = GetAttribute(attributeName, caseSensitive);
    if (attribute == nullptr)
    {
        return std::string();
    }

    return attribute->attributeValue;
}

bool GB_XmlNode::HasChild(const std::string& childName, bool caseSensitive) const
{
    return GetChild(childName, caseSensitive) != nullptr;
}

const GB_XmlNode* GB_XmlNode::GetChild(const std::string& childName, bool caseSensitive) const
{
    if (childName.empty())
    {
        return nullptr;
    }

    for (std::size_t index = 0; index < children.size(); index++)
    {
        const GB_XmlNode& childNode = children[index];
        if (childNode.nodeType != Type::Element)
        {
            continue;
        }

        if (IsXmlNameMatched(childName, childNode.nodeTag, childNode.localName, caseSensitive))
        {
            return &childNode;
        }
    }

    return nullptr;
}

GB_XmlNode* GB_XmlNode::GetChild(const std::string& childName, bool caseSensitive)
{
    return const_cast<GB_XmlNode*>(static_cast<const GB_XmlNode&>(*this).GetChild(childName, caseSensitive));
}

std::vector<const GB_XmlNode*> GB_XmlNode::GetChildren(const std::string& childName, bool caseSensitive) const
{
    std::vector<const GB_XmlNode*> matchedChildren;

    for (std::size_t index = 0; index < children.size(); index++)
    {
        const GB_XmlNode& childNode = children[index];
        if (childNode.nodeType != Type::Element)
        {
            continue;
        }

        if (!childName.empty() && !IsXmlNameMatched(childName, childNode.nodeTag, childNode.localName, caseSensitive))
        {
            continue;
        }

        matchedChildren.push_back(&childNode);
    }

    return matchedChildren;
}

std::vector<GB_XmlNode*> GB_XmlNode::GetChildren(const std::string& childName, bool caseSensitive)
{
    std::vector<GB_XmlNode*> matchedChildren;

    for (std::size_t index = 0; index < children.size(); index++)
    {
        GB_XmlNode& childNode = children[index];
        if (childNode.nodeType != Type::Element)
        {
            continue;
        }

        if (!childName.empty() && !IsXmlNameMatched(childName, childNode.nodeTag, childNode.localName, caseSensitive))
        {
            continue;
        }

        matchedChildren.push_back(&childNode);
    }

    return matchedChildren;
}

bool GB_XmlNode::TryGetChildValue(const std::string& childName, std::string& outValue, bool caseSensitive) const
{
    const GB_XmlNode* childNode = GetChild(childName, caseSensitive);
    if (childNode == nullptr)
    {
        return false;
    }

    outValue = childNode->GetValue();
    return true;
}

std::string GB_XmlNode::GetChildValue(const std::string& childName, bool caseSensitive) const
{
    const GB_XmlNode* childNode = GetChild(childName, caseSensitive);
    if (childNode == nullptr)
    {
        return std::string();
    }

    return childNode->GetValue();
}

std::string GB_XmlNode::GetValue() const
{
    if (nodeType == Type::Comment || nodeType == Type::ProcessingInstruction)
    {
        return nodeValue;
    }

    std::string value;
    AppendXmlTextValue(*this, value);
    return value;
}

bool GB_XmlParser::ParseToDocument(const std::string& xmlText, GB_XmlDocument& outDocument, const GB_XmlParserOptions& options, std::string* errorMessage)
{
    GB_XmlDocument newDocument;
    if (!ParseXmlDocumentInternal(xmlText, newDocument, options, errorMessage))
    {
        return false;
    }

    outDocument = std::move(newDocument);
    return true;
}

bool GB_XmlParser::ParseToRootNode(const std::string& xmlText, GB_XmlNode& outRootNode, const GB_XmlParserOptions& options, std::string* errorMessage)
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
