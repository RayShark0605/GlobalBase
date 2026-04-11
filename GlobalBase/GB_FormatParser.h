#ifndef GLOBALBASE_FORMAT_PARSER_H_H
#define GLOBALBASE_FORMAT_PARSER_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include "GB_Variant.h"

#include <string>
#include <vector>

class GLOBALBASE_PORT GB_JsonParser
{
public:
    static bool ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage = nullptr);

    static bool ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage = nullptr);
};

struct GB_XmlAttribute
{
    std::string attributeName = "";
    std::string localName = "";
    std::string namespacePrefix = "";
    std::string namespaceUri = "";
    std::string attributeValue = "";
};

struct GB_XmlNamespaceDeclaration
{
    std::string namespacePrefix = "";
    std::string namespaceUri = "";
};

struct GB_XmlDiagnostic
{
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
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

struct GLOBALBASE_PORT GB_XmlNode
{
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

    bool IsElement(const std::string& elementName, bool caseSensitive = false) const;

    bool HasAttribute(const std::string& attributeName, bool caseSensitive = false) const;

    const GB_XmlAttribute* GetAttribute(const std::string& attributeName, bool caseSensitive = false) const;

    GB_XmlAttribute* GetAttribute(const std::string& attributeName, bool caseSensitive = false);

    bool TryGetAttributeValue(const std::string& attributeName, std::string& outValue, bool caseSensitive = false) const;

    std::string GetAttributeValue(const std::string& attributeName, bool caseSensitive = false) const;

    bool HasChild(const std::string& childName, bool caseSensitive = false) const;

    const GB_XmlNode* GetChild(const std::string& childName, bool caseSensitive = false) const;

    GB_XmlNode* GetChild(const std::string& childName, bool caseSensitive = false);

    std::vector<const GB_XmlNode*> GetChildren(const std::string& childName = "", bool caseSensitive = false) const;

    std::vector<GB_XmlNode*> GetChildren(const std::string& childName = "", bool caseSensitive = false);

    bool TryGetChildValue(const std::string& childName, std::string& outValue, bool caseSensitive = false) const;

    std::string GetChildValue(const std::string& childName, bool caseSensitive = false) const;

    std::string GetValue() const;
};

struct GB_XmlDocument
{
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

struct GB_XmlParserOptions
{
    bool allowRecovery = false;
    bool preserveWhitespaceOnlyTextNodes = false;
    bool preserveCDataSections = true;
    bool preserveComments = true;
    bool preserveProcessingInstructions = true;
    bool preserveEntityReferences = true;
    bool includeNamespaceDeclarations = true;
    bool cleanRedundantNamespaceDeclarations = false;
    bool substituteEntities = false;
    bool loadExternalDtd = false;
    bool applyDefaultDtdAttributes = false;
    bool validateWithDtd = false;
    bool allowExternalEntities = false;
    bool allowNetworkAccess = false;
    bool allowHugeDocuments = false;
    bool compactMemory = true;
    bool reportLargeLineNumbers = true;
    std::string baseUrl = "";
    std::string forcedEncoding = "";
};

class GLOBALBASE_PORT GB_XmlParser
{
public:
    static bool ParseToDocument(const std::string& xmlText, GB_XmlDocument& outDocument, const GB_XmlParserOptions& options = GB_XmlParserOptions(), std::string* errorMessage = nullptr);

    static bool ParseToRootNode(const std::string& xmlText, GB_XmlNode& outRootNode, const GB_XmlParserOptions& options = GB_XmlParserOptions(), std::string* errorMessage = nullptr);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
