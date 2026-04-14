#include "GB_FormatParser.h"

#include "GB_FileSystem.h"
#include "GB_IO.h"

#include "cpl_error.h"
#include "cpl_json.h"

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable: 4251)
# pragma warning(push)
# pragma warning(disable: 4275)
#endif
#include <yaml-cpp/yaml.h>
#ifdef _MSC_VER
# pragma warning(pop)
# pragma warning(pop)
#endif

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlmemory.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <locale.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
# include <windows.h>
#else
# include <sys/stat.h>
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

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

    std::size_t GetUtf8BomLength(const std::string& text)
    {
        return HasUtf8Bom(text) ? kUtf8BomLength : 0;
    }

    bool StartsWithXmlDeclaration(const std::string& text)
    {
        const std::size_t bomLength = GetUtf8BomLength(text);
        return text.size() >= bomLength + 5
            && text.compare(bomLength, 5, "<?xml") == 0;
    }

#ifdef _WIN32
    bool TryConvertUtf8ToWideString(const std::string& text, std::wstring& outWideText)
    {
        outWideText.clear();

        if (text.empty())
        {
            return true;
        }

        const int requiredLength = MultiByteToWideChar(CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.c_str(),
            static_cast<int>(text.size()),
            nullptr,
            0);
        if (requiredLength <= 0)
        {
            return false;
        }

        try
        {
            outWideText.resize(static_cast<std::size_t>(requiredLength));
        }
        catch (...)
        {
            outWideText.clear();
            return false;
        }

        const int convertedLength = MultiByteToWideChar(CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.c_str(),
            static_cast<int>(text.size()),
            &outWideText[0],
            requiredLength);
        if (convertedLength != requiredLength)
        {
            outWideText.clear();
            return false;
        }

        return true;
    }
#endif

    bool TryGetRegularFileSize(const std::string& filePath, std::uint64_t& outFileSize)
    {
        outFileSize = 0;

#ifdef _WIN32
        std::wstring wideFilePath;
        if (!TryConvertUtf8ToWideString(filePath, wideFilePath) || wideFilePath.empty())
        {
            return false;
        }

        WIN32_FILE_ATTRIBUTE_DATA fileAttributeData;
        if (!GetFileAttributesExW(wideFilePath.c_str(), GetFileExInfoStandard, &fileAttributeData))
        {
            return false;
        }

        if ((fileAttributeData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        ULARGE_INTEGER fileSize;
        fileSize.LowPart = fileAttributeData.nFileSizeLow;
        fileSize.HighPart = fileAttributeData.nFileSizeHigh;
        outFileSize = static_cast<std::uint64_t>(fileSize.QuadPart);
        return true;
#else
        struct stat fileStatus;
        if (stat(filePath.c_str(), &fileStatus) != 0)
        {
            return false;
        }

        if (!S_ISREG(fileStatus.st_mode))
        {
            return false;
        }

        outFileSize = static_cast<std::uint64_t>(fileStatus.st_size);
        return true;
#endif
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
                SetErrorMessage(errorMessage, "Failed to parse JSON text.\nPlease ensure the input is valid JSON.");
            }

            return false;
        }

        return true;
    }

    bool ReadTextFile(const std::string& filePath, std::string& outText, std::string* errorMessage)
    {
        if (filePath.empty())
        {
            SetErrorMessage(errorMessage, "Input file path is empty.");
            return false;
        }

        try
        {
            if (!GB_IsFileExists(filePath))
            {
                SetErrorMessage(errorMessage, "Failed to open file: " + filePath + ".");
                return false;
            }

            std::uint64_t fileSize = 0;
            const bool hasKnownFileSize = TryGetRegularFileSize(filePath, fileSize);

            GB_ByteBuffer fileData = GB_ReadFileToBinary(filePath);
            if (fileData.empty())
            {
                if (hasKnownFileSize && fileSize == 0)
                {
                    outText.clear();
                    return true;
                }

                SetErrorMessage(errorMessage, "Failed to read file: " + filePath + ".");
                return false;
            }

            outText = GB_ByteBufferToString(std::move(fileData));
            return true;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to read file " + filePath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to read file " + filePath + " due to an unknown exception.");
            return false;
        }
    }

    std::string GetYamlNodeTypeName(const YAML::NodeType::value type)
    {
        switch (type)
        {
        case YAML::NodeType::Undefined:
            return "Undefined";

        case YAML::NodeType::Null:
            return "Null";

        case YAML::NodeType::Scalar:
            return "Scalar";

        case YAML::NodeType::Sequence:
            return "Sequence";

        case YAML::NodeType::Map:
            return "Map";

        default:
            return "Unknown";
        }
    }

    bool IsAsciiWhitespace(const unsigned char character)
    {
        return character == ' '
            || character == '\t'
            || character == '\n'
            || character == '\r'
            || character == '\f'
            || character == '\v';
    }

    std::string TrimAsciiText(const std::string& text)
    {
        std::size_t beginIndex = 0;
        std::size_t endIndex = text.size();

        while (beginIndex < endIndex && IsAsciiWhitespace(static_cast<unsigned char>(text[beginIndex])))
        {
            beginIndex++;
        }

        while (endIndex > beginIndex && IsAsciiWhitespace(static_cast<unsigned char>(text[endIndex - 1])))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    std::string ToLowerAsciiText(std::string text)
    {
        for (std::size_t index = 0; index < text.size(); index++)
        {
            char& currentChar = text[index];
            if (currentChar >= 'A' && currentChar <= 'Z')
            {
                currentChar = static_cast<char>(currentChar - 'A' + 'a');
            }
        }

        return text;
    }

    int GetYamlDigitValue(const char character)
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }

        if (character >= 'a' && character <= 'z')
        {
            return character - 'a' + 10;
        }

        if (character >= 'A' && character <= 'Z')
        {
            return character - 'A' + 10;
        }

        return -1;
    }

    bool IsValidYamlDigitForBase(const char character, const unsigned int numericBase)
    {
        const int digitValue = GetYamlDigitValue(character);
        return digitValue >= 0 && static_cast<unsigned int>(digitValue) < numericBase;
    }

    bool ContainsYamlNumericSeparator(const std::string& text);

    bool TryNormalizeYamlIntegerText(const std::string& text,
        bool& isNegative,
        unsigned int& numericBase,
        std::string& outDigits)
    {
        const std::string trimmedText = TrimAsciiText(text);
        if (trimmedText.empty())
        {
            return false;
        }

        if (ContainsYamlNumericSeparator(trimmedText))
        {
            return false;
        }

        std::size_t digitBeginIndex = 0;
        isNegative = false;
        if (trimmedText[0] == '+' || trimmedText[0] == '-')
        {
            if (trimmedText.size() == 1)
            {
                return false;
            }

            isNegative = trimmedText[0] == '-';
            digitBeginIndex = 1;
        }

        numericBase = 10;
        if (trimmedText.size() >= digitBeginIndex + 2 && trimmedText[digitBeginIndex] == '0')
        {
            const char prefixChar = trimmedText[digitBeginIndex + 1];
            if (prefixChar == 'x' || prefixChar == 'X')
            {
                numericBase = 16;
                digitBeginIndex += 2;
            }
            else if (prefixChar == 'o' || prefixChar == 'O')
            {
                numericBase = 8;
                digitBeginIndex += 2;
            }
        }

        if (digitBeginIndex >= trimmedText.size())
        {
            return false;
        }

        outDigits.clear();
        outDigits.reserve(trimmedText.size() - digitBeginIndex);

        for (std::size_t index = digitBeginIndex; index < trimmedText.size(); index++)
        {
            const char currentChar = trimmedText[index];
            if (!IsValidYamlDigitForBase(currentChar, numericBase))
            {
                return false;
            }

            outDigits.push_back(currentChar);
        }

        return !outDigits.empty();
    }

    bool TryAccumulateYamlUnsignedInteger(const unsigned long long currentValue,
        const unsigned int numericBase,
        const unsigned int digitValue,
        unsigned long long& outValue)
    {
        if (currentValue > (std::numeric_limits<unsigned long long>::max() - digitValue) / numericBase)
        {
            return false;
        }

        outValue = currentValue * numericBase + digitValue;
        return true;
    }

    bool TryParseYamlUnsignedMagnitude(const std::string& digits,
        const unsigned int numericBase,
        unsigned long long& outValue)
    {
        if (digits.empty())
        {
            return false;
        }

        unsigned long long currentValue = 0;
        for (std::size_t index = 0; index < digits.size(); index++)
        {
            const int digitValue = GetYamlDigitValue(digits[index]);
            if (digitValue < 0 || static_cast<unsigned int>(digitValue) >= numericBase)
            {
                return false;
            }

            if (!TryAccumulateYamlUnsignedInteger(currentValue,
                numericBase,
                static_cast<unsigned int>(digitValue),
                currentValue))
            {
                return false;
            }
        }

        outValue = currentValue;
        return true;
    }

    bool TryParseYamlSignedInteger(const std::string& text, long long& outValue)
    {
        bool isNegative = false;
        unsigned int numericBase = 10;
        std::string digits;
        if (!TryNormalizeYamlIntegerText(text, isNegative, numericBase, digits))
        {
            return false;
        }

        unsigned long long magnitudeValue = 0;
        if (!TryParseYamlUnsignedMagnitude(digits, numericBase, magnitudeValue))
        {
            return false;
        }

        if (isNegative)
        {
            const unsigned long long minSignedMagnitude = static_cast<unsigned long long>(std::numeric_limits<long long>::max()) + 1ULL;
            if (magnitudeValue > minSignedMagnitude)
            {
                return false;
            }

            if (magnitudeValue == minSignedMagnitude)
            {
                outValue = std::numeric_limits<long long>::min();
            }
            else
            {
                outValue = -static_cast<long long>(magnitudeValue);
            }

            return true;
        }

        if (magnitudeValue > static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
        {
            return false;
        }

        outValue = static_cast<long long>(magnitudeValue);
        return true;
    }

    bool TryParseYamlUnsignedInteger(const std::string& text, unsigned long long& outValue)
    {
        bool isNegative = false;
        unsigned int numericBase = 10;
        std::string digits;
        if (!TryNormalizeYamlIntegerText(text, isNegative, numericBase, digits) || isNegative)
        {
            return false;
        }

        return TryParseYamlUnsignedMagnitude(digits, numericBase, outValue);
    }

    bool LooksLikeYamlFloatingPoint(const std::string& text)
    {
        const std::string lowerText = ToLowerAsciiText(TrimAsciiText(text));
        if (lowerText.empty())
        {
            return false;
        }

        if (lowerText == ".nan" || lowerText == "+.nan" || lowerText == "-.nan"
            || lowerText == ".inf" || lowerText == "+.inf" || lowerText == "-.inf")
        {
            return true;
        }

        return lowerText.find('.') != std::string::npos
            || lowerText.find('e') != std::string::npos;
    }

    bool ContainsYamlNumericSeparator(const std::string& text)
    {
        return text.find('_') != std::string::npos;
    }

    /**
     * @brief 与 locale 无关的 strtod 封装。
     *
     * 标准 std::strtod 依赖当前 C locale 的小数点字符（如法语 locale 使用逗号），
     * 而 YAML 规范固定使用 '.' 作为小数点分隔符。此函数使用平台特定的 locale-aware
     * strtod 变体，强制以 "C" locale 进行解析，避免 locale 导致的浮点解析错误。
     */
    double StrtodWithCLocale(const char* text, char** endPointer)
    {
#ifdef _WIN32
        static const _locale_t cLocale = _create_locale(LC_NUMERIC, "C");
        if (cLocale != nullptr)
        {
            return _strtod_l(text, endPointer, cLocale);
        }

        return std::strtod(text, endPointer);
#elif defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        static const locale_t cLocale = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
        if (cLocale != static_cast<locale_t>(0))
        {
            return strtod_l(text, endPointer, cLocale);
        }

        return std::strtod(text, endPointer);
#else
        return std::strtod(text, endPointer);
#endif
    }

    bool TryParseYamlDouble(const std::string& text, double& outValue)
    {
        const std::string trimmedText = TrimAsciiText(text);
        if (trimmedText.empty())
        {
            return false;
        }

        const std::string lowerText = ToLowerAsciiText(trimmedText);
        if (lowerText == ".nan" || lowerText == "+.nan" || lowerText == "-.nan")
        {
            outValue = std::numeric_limits<double>::quiet_NaN();
            return true;
        }

        if (lowerText == ".inf" || lowerText == "+.inf")
        {
            outValue = std::numeric_limits<double>::infinity();
            return true;
        }

        if (lowerText == "-.inf")
        {
            outValue = -std::numeric_limits<double>::infinity();
            return true;
        }

        if (ContainsYamlNumericSeparator(trimmedText))
        {
            return false;
        }

        errno = 0;
        char* endPointer = nullptr;
        const double parsedValue = StrtodWithCLocale(trimmedText.c_str(), &endPointer);
        if (errno == ERANGE || endPointer == trimmedText.c_str() || endPointer == nullptr || *endPointer != '\0')
        {
            return false;
        }

        outValue = parsedValue;
        return true;
    }

    bool TryParseYamlBoolean(const std::string& text, bool& outValue)
    {
        const std::string lowerText = ToLowerAsciiText(TrimAsciiText(text));
        if (lowerText.empty())
        {
            return false;
        }

        if (lowerText == "true")
        {
            outValue = true;
            return true;
        }

        if (lowerText == "false")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    bool IsYamlExplicitStringTag(const std::string& tagText)
    {
        return tagText == "!!str"
            || tagText == "!<tag:yaml.org,2002:str>"
            || tagText == "tag:yaml.org,2002:str";
    }

    bool IsYamlExplicitNullTag(const std::string& tagText)
    {
        return tagText == "!!null"
            || tagText == "!<tag:yaml.org,2002:null>"
            || tagText == "tag:yaml.org,2002:null";
    }

    bool IsYamlExplicitBoolTag(const std::string& tagText)
    {
        return tagText == "!!bool"
            || tagText == "!<tag:yaml.org,2002:bool>"
            || tagText == "tag:yaml.org,2002:bool";
    }

    bool IsYamlExplicitIntTag(const std::string& tagText)
    {
        return tagText == "!!int"
            || tagText == "!<tag:yaml.org,2002:int>"
            || tagText == "tag:yaml.org,2002:int";
    }

    bool IsYamlExplicitFloatTag(const std::string& tagText)
    {
        return tagText == "!!float"
            || tagText == "!<tag:yaml.org,2002:float>"
            || tagText == "tag:yaml.org,2002:float";
    }

    long long GetYamlLineNumber(const YAML::Mark& mark)
    {
        return mark.is_null() || mark.line < 0 ? -1 : static_cast<long long>(mark.line) + 1;
    }

    int GetYamlColumnNumber(const YAML::Mark& mark)
    {
        return mark.is_null() || mark.column < 0 ? -1 : mark.column + 1;
    }

    std::string BuildYamlExceptionSummary(const YAML::Exception& exceptionObject)
    {
        std::ostringstream stream;
        stream << "Failed to parse YAML";

        const long long lineNumber = GetYamlLineNumber(exceptionObject.mark);
        const int columnNumber = GetYamlColumnNumber(exceptionObject.mark);
        if (lineNumber > 0)
        {
            stream << " at line " << lineNumber;
            if (columnNumber > 0)
            {
                stream << ", column " << columnNumber;
            }
        }

        if (!exceptionObject.msg.empty())
        {
            stream << ": " << exceptionObject.msg;
        }
        else
        {
            stream << ".";
        }

        return stream.str();
    }

    std::string GetYamlMapChildPath(const std::string& parentPath, const std::string& childName)
    {
        return parentPath + "[\"" + EscapeJsonPathText(childName) + "\"]";
    }

    std::string GetYamlSequenceItemPath(const std::string& parentPath, const std::size_t index)
    {
        return parentPath + "[" + std::to_string(index) + "]";
    }

    bool TryGetYamlNodeTag(const YAML::Node& node, std::string& outTag)
    {
        try
        {
            outTag = node.Tag();
            return true;
        }
        catch (...)
        {
            outTag.clear();
            return false;
        }
    }

    bool IsYamlCanonicalNullText(const std::string& trimmedText)
    {
        const std::string lowerText = ToLowerAsciiText(trimmedText);
        return trimmedText.empty()
            || lowerText == "null"
            || lowerText == "~";
    }

    bool HasYamlExplicitTag(const std::string& tagText)
    {
        return !tagText.empty() && tagText != "!" && tagText != "?";
    }

    bool AssignYamlIntegerVariant(const long long signedIntegerValue,
        const unsigned long long unsignedIntegerValue,
        const bool useUnsignedValue,
        GB_Variant& outValue)
    {
        if (useUnsignedValue)
        {
            if (unsignedIntegerValue <= static_cast<unsigned long long>(std::numeric_limits<unsigned int>::max()))
            {
                outValue = static_cast<unsigned int>(unsignedIntegerValue);
            }
            else if (unsignedIntegerValue <= static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            {
                outValue = static_cast<long long>(unsignedIntegerValue);
            }
            else
            {
                outValue = unsignedIntegerValue;
            }

            return true;
        }

        if (signedIntegerValue >= static_cast<long long>(std::numeric_limits<int>::min())
            && signedIntegerValue <= static_cast<long long>(std::numeric_limits<int>::max()))
        {
            outValue = static_cast<int>(signedIntegerValue);
        }
        else
        {
            outValue = signedIntegerValue;
        }

        return true;
    }

    bool TryConvertYamlScalarTextToVariant(const std::string& scalarText,
        const std::string& tagText,
        const GB_YamlParserOptions& options,
        GB_Variant& outValue,
        std::string* errorMessage,
        const std::string& yamlPath)
    {
        if (IsYamlExplicitStringTag(tagText))
        {
            outValue = scalarText;
            return true;
        }

        const std::string trimmedText = TrimAsciiText(scalarText);
        const std::string lowerText = ToLowerAsciiText(trimmedText);
        const bool isExplicitNullTag = IsYamlExplicitNullTag(tagText);
        const bool isExplicitBoolTag = IsYamlExplicitBoolTag(tagText);
        const bool isExplicitIntTag = IsYamlExplicitIntTag(tagText);
        const bool isExplicitFloatTag = IsYamlExplicitFloatTag(tagText);

        if (isExplicitNullTag)
        {
            if (!IsYamlCanonicalNullText(trimmedText))
            {
                SetErrorMessage(errorMessage, "YAML scalar at " + yamlPath + " is tagged as !!null but contains non-null text.");
                return false;
            }

            outValue.Reset();
            return true;
        }

        if (isExplicitBoolTag)
        {
            bool boolValue = false;
            if (!TryParseYamlBoolean(trimmedText, boolValue))
            {
                SetErrorMessage(errorMessage, "YAML scalar at " + yamlPath + " is tagged as !!bool but cannot be parsed as a boolean.");
                return false;
            }

            outValue = boolValue;
            return true;
        }

        if (isExplicitIntTag)
        {
            long long signedIntegerValue = 0;
            if (TryParseYamlSignedInteger(trimmedText, signedIntegerValue))
            {
                return AssignYamlIntegerVariant(signedIntegerValue, 0ULL, false, outValue);
            }

            unsigned long long unsignedIntegerValue = 0;
            if (TryParseYamlUnsignedInteger(trimmedText, unsignedIntegerValue))
            {
                return AssignYamlIntegerVariant(0LL, unsignedIntegerValue, true, outValue);
            }

            SetErrorMessage(errorMessage, "YAML scalar at " + yamlPath + " is tagged as !!int but cannot be parsed as an integer.");
            return false;
        }

        if (isExplicitFloatTag)
        {
            double doubleValue = 0.0;
            if (!TryParseYamlDouble(trimmedText, doubleValue))
            {
                SetErrorMessage(errorMessage, "YAML scalar at " + yamlPath + " is tagged as !!float but cannot be parsed as a floating-point value.");
                return false;
            }

            outValue = doubleValue;
            return true;
        }

        if (HasYamlExplicitTag(tagText))
        {
            outValue = scalarText;
            return true;
        }

        if (!options.autoConvertScalarValues)
        {
            outValue = scalarText;
            return true;
        }

        if (lowerText == "null" || lowerText == "~")
        {
            outValue.Reset();
            return true;
        }

        bool boolValue = false;
        if (TryParseYamlBoolean(trimmedText, boolValue))
        {
            outValue = boolValue;
            return true;
        }

        if (!trimmedText.empty())
        {
            long long signedIntegerValue = 0;
            if (TryParseYamlSignedInteger(trimmedText, signedIntegerValue))
            {
                return AssignYamlIntegerVariant(signedIntegerValue, 0ULL, false, outValue);
            }

            unsigned long long unsignedIntegerValue = 0;
            if (TryParseYamlUnsignedInteger(trimmedText, unsignedIntegerValue))
            {
                return AssignYamlIntegerVariant(0LL, unsignedIntegerValue, true, outValue);
            }
        }

        if (LooksLikeYamlFloatingPoint(trimmedText))
        {
            double doubleValue = 0.0;
            if (TryParseYamlDouble(trimmedText, doubleValue))
            {
                outValue = doubleValue;
                return true;
            }
        }

        outValue = scalarText;
        return true;
    }

    bool IsYamlNodeOnActivePath(const YAML::Node& node, const std::vector<YAML::Node>& activeNodes)
    {
        for (std::size_t index = 0; index < activeNodes.size(); index++)
        {
            if (node.is(activeNodes[index]))
            {
                return true;
            }
        }

        return false;
    }

    class YamlActiveNodeScope
    {
    public:
        YamlActiveNodeScope(std::vector<YAML::Node>* activeNodes, const YAML::Node& node, const bool enabled)
            : activeNodes_(activeNodes), enabled_(enabled)
        {
            if (enabled_ && activeNodes_ != nullptr)
            {
                activeNodes_->push_back(node);
            }
        }

        ~YamlActiveNodeScope()
        {
            if (enabled_ && activeNodes_ != nullptr)
            {
                activeNodes_->pop_back();
            }
        }

    private:
        std::vector<YAML::Node>* activeNodes_ = nullptr;
        bool enabled_ = false;
    };

    bool ConvertYamlNodeToVariant(const YAML::Node& yamlNode,
        GB_Variant& outValue,
        const GB_YamlParserOptions& options,
        std::string* errorMessage,
        const std::string& yamlPath,
        const int currentDepth,
        std::vector<YAML::Node>& activeNodes);

    bool ConvertYamlMapToVariantMap(const YAML::Node& yamlNode,
        GB_VariantMap& outMap,
        const GB_YamlParserOptions& options,
        std::string* errorMessage,
        const std::string& yamlPath,
        const int currentDepth,
        std::vector<YAML::Node>& activeNodes)
    {
        if (!yamlNode.IsMap())
        {
            SetErrorMessage(errorMessage, "YAML node at " + yamlPath + " is not a mapping. Actual type: " + GetYamlNodeTypeName(yamlNode.Type()) + ".");
            return false;
        }

        if (currentDepth > options.maxNestingDepth)
        {
            SetErrorMessage(errorMessage, "YAML nesting depth exceeds the supported limit (" + std::to_string(options.maxNestingDepth) + ") at " + yamlPath + ".");
            return false;
        }

        if (IsYamlNodeOnActivePath(yamlNode, activeNodes))
        {
            SetErrorMessage(errorMessage, "Detected a cyclic YAML alias/reference at " + yamlPath + ". The current GB_Variant-based conversion does not support cyclic graphs.");
            return false;
        }

        try
        {
            YamlActiveNodeScope activeNodeScope(&activeNodes, yamlNode, true);

            GB_VariantMap newMap;
            for (YAML::const_iterator iterator = yamlNode.begin(); iterator != yamlNode.end(); ++iterator)
            {
                const YAML::Node keyNode = iterator->first;
                const YAML::Node valueNode = iterator->second;

                std::string keyText;
                if (keyNode.IsScalar())
                {
                    keyText = keyNode.Scalar();
                }
                else if (options.stringifyNonScalarMapKeys)
                {
                    keyText = YAML::Dump(keyNode);
                }
                else
                {
                    SetErrorMessage(errorMessage, "YAML mapping key at " + yamlPath + " is not a scalar. GB_VariantMap only supports std::string keys. Enable stringifyNonScalarMapKeys to serialize complex keys into YAML text.");
                    return false;
                }

                const std::string childPath = GetYamlMapChildPath(yamlPath, keyText);
                GB_Variant childValue;
                if (!ConvertYamlNodeToVariant(valueNode, childValue, options, errorMessage, childPath, currentDepth + 1, activeNodes))
                {
                    return false;
                }

                const GB_VariantMap::iterator insertPosition = newMap.lower_bound(keyText);
                if (insertPosition != newMap.end() && insertPosition->first == keyText)
                {
                    if (!options.allowDuplicateMapKeys)
                    {
                        SetErrorMessage(errorMessage, "Duplicate YAML mapping key encountered after conversion at " + childPath + ".");
                        return false;
                    }

                    insertPosition->second = std::move(childValue);
                }
                else
                {
                    newMap.emplace_hint(insertPosition, keyText, std::move(childValue));
                }
            }

            outMap = std::move(newMap);
            return true;
        }
        catch (const YAML::Exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, BuildYamlExceptionSummary(exceptionObject));
            return false;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML mapping at " + yamlPath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML mapping at " + yamlPath + " due to an unknown exception.");
            return false;
        }
    }

    bool ConvertYamlSequenceToVariantList(const YAML::Node& yamlNode,
        GB_VariantList& outList,
        const GB_YamlParserOptions& options,
        std::string* errorMessage,
        const std::string& yamlPath,
        const int currentDepth,
        std::vector<YAML::Node>& activeNodes)
    {
        if (!yamlNode.IsSequence())
        {
            SetErrorMessage(errorMessage, "YAML node at " + yamlPath + " is not a sequence. Actual type: " + GetYamlNodeTypeName(yamlNode.Type()) + ".");
            return false;
        }

        if (currentDepth > options.maxNestingDepth)
        {
            SetErrorMessage(errorMessage, "YAML nesting depth exceeds the supported limit (" + std::to_string(options.maxNestingDepth) + ") at " + yamlPath + ".");
            return false;
        }

        if (IsYamlNodeOnActivePath(yamlNode, activeNodes))
        {
            SetErrorMessage(errorMessage, "Detected a cyclic YAML alias/reference at " + yamlPath + ". The current GB_Variant-based conversion does not support cyclic graphs.");
            return false;
        }

        try
        {
            YamlActiveNodeScope activeNodeScope(&activeNodes, yamlNode, true);

            GB_VariantList newList;
            const std::size_t itemCount = yamlNode.size();
            if (itemCount > 0)
            {
                newList.reserve(itemCount);
            }

            std::size_t itemIndex = 0;
            for (YAML::const_iterator iterator = yamlNode.begin(); iterator != yamlNode.end(); ++iterator, itemIndex++)
            {
                GB_Variant itemValue;
                if (!ConvertYamlNodeToVariant(*iterator, itemValue, options, errorMessage, GetYamlSequenceItemPath(yamlPath, itemIndex), currentDepth + 1, activeNodes))
                {
                    return false;
                }

                newList.push_back(std::move(itemValue));
            }

            outList = std::move(newList);
            return true;
        }
        catch (const YAML::Exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, BuildYamlExceptionSummary(exceptionObject));
            return false;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML sequence at " + yamlPath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML sequence at " + yamlPath + " due to an unknown exception.");
            return false;
        }
    }

    bool ConvertYamlNodeToVariant(const YAML::Node& yamlNode,
        GB_Variant& outValue,
        const GB_YamlParserOptions& options,
        std::string* errorMessage,
        const std::string& yamlPath,
        const int currentDepth,
        std::vector<YAML::Node>& activeNodes)
    {
        if (currentDepth > options.maxNestingDepth)
        {
            SetErrorMessage(errorMessage, "YAML nesting depth exceeds the supported limit (" + std::to_string(options.maxNestingDepth) + ") at " + yamlPath + ".");
            return false;
        }

        try
        {
            if (!yamlNode.IsDefined() || yamlNode.Type() == YAML::NodeType::Undefined)
            {
                outValue.Reset();
                return true;
            }

            switch (yamlNode.Type())
            {
            case YAML::NodeType::Null:
                outValue.Reset();
                return true;

            case YAML::NodeType::Scalar:
            {
                std::string tagText;
                TryGetYamlNodeTag(yamlNode, tagText);
                return TryConvertYamlScalarTextToVariant(yamlNode.Scalar(), tagText, options, outValue, errorMessage, yamlPath);
            }

            case YAML::NodeType::Sequence:
            {
                GB_VariantList listValue;
                if (!ConvertYamlSequenceToVariantList(yamlNode, listValue, options, errorMessage, yamlPath, currentDepth, activeNodes))
                {
                    return false;
                }

                outValue = std::move(listValue);
                return true;
            }

            case YAML::NodeType::Map:
            {
                GB_VariantMap mapValue;
                if (!ConvertYamlMapToVariantMap(yamlNode, mapValue, options, errorMessage, yamlPath, currentDepth, activeNodes))
                {
                    return false;
                }

                outValue = std::move(mapValue);
                return true;
            }

            case YAML::NodeType::Undefined:
                outValue.Reset();
                return true;

            default:
                SetErrorMessage(errorMessage, "Unsupported YAML node type at " + yamlPath + ": " + GetYamlNodeTypeName(yamlNode.Type()) + ".");
                return false;
            }
        }
        catch (const YAML::Exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, BuildYamlExceptionSummary(exceptionObject));
            return false;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML node at " + yamlPath + ": " + exceptionObject.what());
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert YAML node at " + yamlPath + " due to an unknown exception.");
            return false;
        }
    }

    bool ConvertYamlDocumentNode(const YAML::Node& yamlNode,
        GB_YamlDocument& outDocument,
        const GB_YamlParserOptions& options,
        std::string* errorMessage)
    {
        try
        {
            GB_YamlDocument newDocument;
            const YAML::Mark rootMark = yamlNode.Mark();
            newDocument.rootLineNumber = GetYamlLineNumber(rootMark);
            newDocument.rootColumnNumber = GetYamlColumnNumber(rootMark);
            TryGetYamlNodeTag(yamlNode, newDocument.rootTag);

            std::vector<YAML::Node> activeNodes;
            if (!ConvertYamlNodeToVariant(yamlNode, newDocument.rootValue, options, errorMessage, "$", 0, activeNodes))
            {
                return false;
            }

            outDocument = std::move(newDocument);
            return true;
        }
        catch (const YAML::Exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, BuildYamlExceptionSummary(exceptionObject));
            return false;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to convert parsed YAML document: " + std::string(exceptionObject.what()));
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to convert parsed YAML document due to an unknown exception.");
            return false;
        }
    }

    bool ParseYamlStreamInternal(const std::string& yamlText, GB_YamlStream& outStream, const GB_YamlParserOptions& options, std::string* errorMessage)
    {
        if (yamlText.empty())
        {
            SetErrorMessage(errorMessage, "Input YAML text is empty.");
            return false;
        }

        if (options.maxNestingDepth < 0)
        {
            SetErrorMessage(errorMessage, "maxNestingDepth cannot be negative.");
            return false;
        }

        const std::string* yamlTextToLoad = &yamlText;
        std::string yamlTextWithoutBom;
        if (HasUtf8Bom(yamlText))
        {
            yamlTextWithoutBom.assign(yamlText.begin() + static_cast<std::ptrdiff_t>(kUtf8BomLength), yamlText.end());
            yamlTextToLoad = &yamlTextWithoutBom;
        }

        if (yamlTextToLoad->empty())
        {
            SetErrorMessage(errorMessage, "Input YAML text is empty.");
            return false;
        }

        try
        {
            const std::vector<YAML::Node> yamlDocuments = YAML::LoadAll(*yamlTextToLoad);
            if (yamlDocuments.empty())
            {
                SetErrorMessage(errorMessage, "Input YAML text does not contain any document.");
                return false;
            }

            GB_YamlStream newStream;
            newStream.documents.reserve(yamlDocuments.size());
            for (std::size_t index = 0; index < yamlDocuments.size(); index++)
            {
                GB_YamlDocument document;
                if (!ConvertYamlDocumentNode(yamlDocuments[index], document, options, errorMessage))
                {
                    return false;
                }

                newStream.documents.push_back(std::move(document));
            }

            outStream = std::move(newStream);
            ClearErrorMessage(errorMessage);
            return true;
        }
        catch (const YAML::Exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, BuildYamlExceptionSummary(exceptionObject));
            return false;
        }
        catch (const std::exception& exceptionObject)
        {
            SetErrorMessage(errorMessage, "Failed to parse YAML text: " + std::string(exceptionObject.what()));
            return false;
        }
        catch (...)
        {
            SetErrorMessage(errorMessage, "Failed to parse YAML text due to an unknown exception.");
            return false;
        }
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

    using UniqueXmlParserContext = std::unique_ptr<xmlParserCtxt, XmlParserContextDeleter>;
    using UniqueXmlDocument = std::unique_ptr<xmlDoc, XmlDocumentDeleter>;

    xmlFreeFunc GetLibXmlFreeFunc()
    {
        static std::once_flag initOnce;
        static xmlFreeFunc freeFunc = nullptr;

        std::call_once(initOnce, []()
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

    using UniqueXmlChar = std::unique_ptr<xmlChar, XmlCharDeleter>;

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
                    const bool sameElementName = currentNode->type != XML_ELEMENT_NODE || xmlStrEqual(sibling->name, currentNode->name);
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
            && (options.substituteEntities || options.loadExternalDtd || options.applyDefaultDtdAttributes || options.validateWithDtd);
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
        UniqueXmlChar attributeValue(xmlNodeListGetString(attributeDocument, attribute->children, 1));
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

    std::size_t GetXmlPreservedChildNodeCount(const xmlNodePtr parentNode, const GB_XmlParserOptions& options)
    {
        std::size_t preservedChildNodeCount = 0;
        for (xmlNodePtr child = parentNode != nullptr ? parentNode->children : nullptr; child != nullptr; child = child->next)
        {
            if (!ShouldSkipXmlChildNode(child, options))
            {
                preservedChildNodeCount++;
            }
        }

        return preservedChildNodeCount;
    }

    long long GetXmlLineNumber(const xmlNodePtr node)
    {
        if (node == nullptr)
        {
            return -1;
        }

        const long lineNumber = xmlGetLineNo(node);
        return lineNumber > 0 ? static_cast<long long>(lineNumber) : -1;
    }

    bool ConvertXmlNode(const xmlNodePtr node, GB_XmlNode& outNode, const GB_XmlParserOptions& options, std::string* errorMessage)
    {
        if (node == nullptr)
        {
            SetErrorMessage(errorMessage, "Cannot convert a null XML node.");
            return false;
        }

        try
        {
            GB_XmlNode newNode;
            newNode.lineNumber = GetXmlLineNumber(node);

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

                const std::size_t preservedChildNodeCount = GetXmlPreservedChildNodeCount(node, options);
                if (preservedChildNodeCount > 0)
                {
                    newNode.children.reserve(preservedChildNodeCount);
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
            {
                newNode.nodeType = GB_XmlNode::Type::EntityReference;
                newNode.nodeTag = XmlCharToString(node->name);
                newNode.localName = newNode.nodeTag;

                const std::size_t preservedChildNodeCount = GetXmlPreservedChildNodeCount(node, options);
                if (preservedChildNodeCount > 0)
                {
                    newNode.children.reserve(preservedChildNodeCount);
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
            default:
                SetErrorMessage(errorMessage, "Unsupported XML node type at " + GetXmlNodePath(node) + ". libxml2 node type: " + std::to_string(static_cast<int>(node->type)) + ".");
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

    void LoadDocumentTypeInfo(const xmlDocPtr xmlDocument, const bool hasXmlDeclaration, GB_XmlDocument& outDocument)
    {
        if (xmlDocument == nullptr)
        {
            return;
        }

        outDocument.version = XmlCharToString(xmlDocument->version);
        outDocument.encoding = XmlCharToString(xmlDocument->encoding);

        if (!hasXmlDeclaration)
        {
            outDocument.standalone = GB_XmlDocument::StandaloneMode::NoDeclaration;
        }
        else if (xmlDocument->standalone < 0)
        {
            outDocument.standalone = GB_XmlDocument::StandaloneMode::Omitted;
        }
        else if (xmlDocument->standalone == 0)
        {
            outDocument.standalone = GB_XmlDocument::StandaloneMode::No;
        }
        else
        {
            outDocument.standalone = GB_XmlDocument::StandaloneMode::Yes;
        }

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

    bool ConvertXmlDocument(const xmlDocPtr xmlDocument, GB_XmlDocument& outDocument, const GB_XmlParserOptions& options, const XmlErrorCollector& errorCollector, const bool recovered, const bool hasXmlDeclaration, std::string* errorMessage)
    {
        if (xmlDocument == nullptr)
        {
            SetErrorMessage(errorMessage, "Parsed XML document is null.");
            return false;
        }

        try
        {
            GB_XmlDocument newDocument;
            LoadDocumentTypeInfo(xmlDocument, hasXmlDeclaration, newDocument);

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

    bool ParseXmlDocumentInternal(const std::string& xmlText, GB_XmlDocument& outDocument, const GB_XmlParserOptions& options, std::string* errorMessage)
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
            SetErrorMessage(errorMessage, "The requested XML parser options require external DTD/entity loading, but the current libxml2 version does not provide XML_PARSE_NO_XXE.\nPlease either enable allowExternalEntities or disable substituteEntities / loadExternalDtd / applyDefaultDtdAttributes / validateWithDtd.");
            return false;
        }

        EnsureLibXmlInitialized();

#if !(defined(LIBXML_VERSION) && LIBXML_VERSION >= 21300)
        std::unique_lock<std::mutex> structuredErrorHandlerLock(GetXmlStructuredErrorHandlerMutex());
#endif

        XmlErrorCollector errorCollector;

        UniqueXmlParserContext parserContext(xmlNewParserCtxt());
        if (!parserContext)
        {
            SetErrorMessage(errorMessage, "Failed to create libxml2 parser context.");
            return false;
        }

        const XmlStructuredErrorHandlerScope structuredErrorHandlerScope(parserContext.get(), &errorCollector);

        const int parseOptions = BuildLibXmlParseOptions(options);
        const char* const baseUrlText = options.baseUrl.empty() ? nullptr : options.baseUrl.c_str();
        const char* const forcedEncodingText = options.forcedEncoding.empty() ? nullptr : options.forcedEncoding.c_str();

        UniqueXmlDocument xmlDocument(xmlCtxtReadMemory(parserContext.get(), xmlText.data(), static_cast<int>(xmlText.size()), baseUrlText, forcedEncodingText, parseOptions));

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
        const bool hasXmlDeclaration = StartsWithXmlDeclaration(xmlText);
        if (!ConvertXmlDocument(xmlDocument.get(), outDocument, options, errorCollector, recovered, hasXmlDeclaration, errorMessage))
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

    std::size_t GetXmlTextValueLength(const GB_XmlNode& node)
    {
        switch (node.nodeType)
        {
        case GB_XmlNode::Type::Text:
        case GB_XmlNode::Type::CData:
            return node.nodeValue.size();

        case GB_XmlNode::Type::Element:
        case GB_XmlNode::Type::EntityReference:
        {
            std::size_t totalLength = 0;
            for (std::size_t index = 0; index < node.children.size(); index++)
            {
                totalLength += GetXmlTextValueLength(node.children[index]);
            }

            return totalLength;
        }

        case GB_XmlNode::Type::Comment:
        case GB_XmlNode::Type::ProcessingInstruction:
        default:
            return 0;
        }
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
    if (attributeName.empty())
    {
        return nullptr;
    }

    for (std::size_t index = 0; index < attributes.size(); index++)
    {
        GB_XmlAttribute& attribute = attributes[index];
        if (IsXmlNameMatched(attributeName, attribute.attributeName, attribute.localName, caseSensitive))
        {
            return &attribute;
        }
    }

    return nullptr;
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
    if (childName.empty())
    {
        return nullptr;
    }

    for (std::size_t index = 0; index < children.size(); index++)
    {
        GB_XmlNode& childNode = children[index];
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

std::vector<const GB_XmlNode*> GB_XmlNode::GetChildren(const std::string& childName, bool caseSensitive) const
{
    std::vector<const GB_XmlNode*> matchedChildren;
    if (!children.empty())
    {
        matchedChildren.reserve(children.size());
    }
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
    if (!children.empty())
    {
        matchedChildren.reserve(children.size());
    }
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
    value.reserve(GetXmlTextValueLength(*this));
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

bool GB_YamlParser::ParseToVariant(const std::string& yamlText, GB_Variant& outValue, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    GB_YamlStream stream;
    if (!ParseYamlStreamInternal(yamlText, stream, options, errorMessage))
    {
        return false;
    }

    if (stream.documents.size() != 1)
    {
        SetErrorMessage(errorMessage, "ParseToVariant requires exactly one YAML document, but " + std::to_string(stream.documents.size()) + " documents were parsed. Use ParseToStream() for multi-document YAML input.");
        return false;
    }

    outValue = std::move(stream.documents[0].rootValue);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_YamlParser::ParseToVariantMap(const std::string& yamlText, GB_VariantMap& outMap, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    GB_Variant value;
    if (!ParseToVariant(yamlText, value, options, errorMessage))
    {
        return false;
    }

    GB_VariantMap* mapValue = value.AnyCast<GB_VariantMap>();
    if (mapValue == nullptr)
    {
        SetErrorMessage(errorMessage, "Parsed YAML root node is not a mapping.");
        return false;
    }

    outMap = std::move(*mapValue);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_YamlParser::ParseToDocument(const std::string& yamlText, GB_YamlDocument& outDocument, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    GB_YamlStream stream;
    if (!ParseYamlStreamInternal(yamlText, stream, options, errorMessage))
    {
        return false;
    }

    if (stream.documents.size() != 1)
    {
        SetErrorMessage(errorMessage, "ParseToDocument requires exactly one YAML document, but " + std::to_string(stream.documents.size()) + " documents were parsed. Use ParseToStream() for multi-document YAML input.");
        return false;
    }

    outDocument = std::move(stream.documents[0]);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_YamlParser::ParseToStream(const std::string& yamlText, GB_YamlStream& outStream, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    GB_YamlStream newStream;
    if (!ParseYamlStreamInternal(yamlText, newStream, options, errorMessage))
    {
        return false;
    }

    outStream = std::move(newStream);
    ClearErrorMessage(errorMessage);
    return true;
}

bool GB_YamlParser::ParseFileToVariant(const std::string& yamlFilePath, GB_Variant& outValue, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    std::string yamlText;
    if (!ReadTextFile(yamlFilePath, yamlText, errorMessage))
    {
        return false;
    }

    if (ParseToVariant(yamlText, outValue, options, errorMessage))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        if (errorMessage->empty())
        {
            *errorMessage = "Failed to parse YAML file: " + yamlFilePath + ".";
        }
        else
        {
            *errorMessage = "Failed to parse YAML file " + yamlFilePath + ": " + *errorMessage;
        }
    }

    return false;
}

bool GB_YamlParser::ParseFileToVariantMap(const std::string& yamlFilePath, GB_VariantMap& outMap, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    std::string yamlText;
    if (!ReadTextFile(yamlFilePath, yamlText, errorMessage))
    {
        return false;
    }

    if (ParseToVariantMap(yamlText, outMap, options, errorMessage))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        if (errorMessage->empty())
        {
            *errorMessage = "Failed to parse YAML file: " + yamlFilePath + ".";
        }
        else
        {
            *errorMessage = "Failed to parse YAML file " + yamlFilePath + ": " + *errorMessage;
        }
    }

    return false;
}

bool GB_YamlParser::ParseFileToDocument(const std::string& yamlFilePath, GB_YamlDocument& outDocument, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    std::string yamlText;
    if (!ReadTextFile(yamlFilePath, yamlText, errorMessage))
    {
        return false;
    }

    if (ParseToDocument(yamlText, outDocument, options, errorMessage))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        if (errorMessage->empty())
        {
            *errorMessage = "Failed to parse YAML file: " + yamlFilePath + ".";
        }
        else
        {
            *errorMessage = "Failed to parse YAML file " + yamlFilePath + ": " + *errorMessage;
        }
    }

    return false;
}

bool GB_YamlParser::ParseFileToStream(const std::string& yamlFilePath, GB_YamlStream& outStream, const GB_YamlParserOptions& options, std::string* errorMessage)
{
    std::string yamlText;
    if (!ReadTextFile(yamlFilePath, yamlText, errorMessage))
    {
        return false;
    }

    if (ParseToStream(yamlText, outStream, options, errorMessage))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        if (errorMessage->empty())
        {
            *errorMessage = "Failed to parse YAML file: " + yamlFilePath + ".";
        }
        else
        {
            *errorMessage = "Failed to parse YAML file " + yamlFilePath + ": " + *errorMessage;
        }
    }

    return false;
}

namespace
{
    bool AreIniNamesEqual(const std::string& leftName, const std::string& rightName, const bool caseSensitive)
    {
        if (caseSensitive)
        {
            return leftName == rightName;
        }

        return boost::iequals(leftName, rightName);
    }

    bool IsIniAsciiWhitespace(const char character)
    {
        const unsigned char unsignedCharacter = static_cast<unsigned char>(character);
        return unsignedCharacter == ' '
            || unsignedCharacter == '\t'
            || unsignedCharacter == '\n'
            || unsignedCharacter == '\r'
            || unsignedCharacter == '\f'
            || unsignedCharacter == '\v';
    }

    std::string_view TrimIniTextView(const std::string_view text)
    {
        std::size_t beginIndex = 0;
        std::size_t endIndex = text.size();

        while (beginIndex < endIndex && IsIniAsciiWhitespace(text[beginIndex]))
        {
            beginIndex++;
        }

        while (endIndex > beginIndex && IsIniAsciiWhitespace(text[endIndex - 1]))
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    std::string TrimIniTextCopy(const std::string& text)
    {
        const std::string_view trimmedText = TrimIniTextView(std::string_view(text));
        return std::string(trimmedText.data(), trimmedText.size());
    }

    struct IniLineNumberItemInfo
    {
        std::string key = "";
        long long lineNumber = -1;
    };

    struct IniLineNumberSectionInfo
    {
        std::string sectionName = "";
        long long lineNumber = -1;
        std::vector<IniLineNumberItemInfo> items;
    };

    struct IniLineNumberCache
    {
        std::vector<IniLineNumberItemInfo> globalItems;
        std::vector<IniLineNumberSectionInfo> sections;
    };

    bool TryParseIniSectionHeader(const std::string_view trimmedLine, std::string& outSectionName)
    {
        outSectionName.clear();

        if (trimmedLine.size() < 2 || trimmedLine[0] != '[')
        {
            return false;
        }

        const std::size_t closeBracketPosition = trimmedLine.find(']');
        if (closeBracketPosition == std::string_view::npos)
        {
            return false;
        }

        const std::string_view trailingText = TrimIniTextView(trimmedLine.substr(closeBracketPosition + 1));
        if (!trailingText.empty() && trailingText[0] != ';' && trailingText[0] != '#')
        {
            return false;
        }

        const std::string_view sectionNameText = TrimIniTextView(trimmedLine.substr(1, closeBracketPosition - 1));
        outSectionName.assign(sectionNameText.data(), sectionNameText.size());
        return true;
    }

    bool TryParseIniKeyName(const std::string_view trimmedLine, std::string& outKey)
    {
        outKey.clear();

        const std::size_t equalSignPosition = trimmedLine.find('=');
        if (equalSignPosition == std::string_view::npos || equalSignPosition == 0)
        {
            return false;
        }

        const std::string_view keyText = TrimIniTextView(trimmedLine.substr(0, equalSignPosition));
        if (keyText.empty())
        {
            return false;
        }

        outKey.assign(keyText.data(), keyText.size());
        return true;
    }

    void AddIniDiagnostic(GB_IniDocument& document, const GB_IniDiagnostic::Level level, const long long lineNumber, const int columnNumber, const std::string& message)
    {
        GB_IniDiagnostic diagnostic;
        diagnostic.level = level;
        diagnostic.lineNumber = lineNumber;
        diagnostic.columnNumber = columnNumber;
        diagnostic.message = message;
        document.diagnostics.push_back(std::move(diagnostic));
    }

    bool FailIniParse(GB_IniDocument& document, std::string* errorMessage, const long long lineNumber, const int columnNumber, const std::string& message)
    {
        AddIniDiagnostic(document, GB_IniDiagnostic::Level::Error, lineNumber, columnNumber, message);

        std::ostringstream stream;
        stream << message;
        if (lineNumber > 0)
        {
            stream << " [line " << lineNumber;
            if (columnNumber > 0)
            {
                stream << ", column " << columnNumber;
            }
            stream << "]";
        }

        SetErrorMessage(errorMessage, stream.str());
        return false;
    }

    bool ConvertIniPropertyTreeToDocument(const boost::property_tree::ptree& propertyTree, GB_IniDocument& outDocument, std::string* errorMessage)
    {
        ClearErrorMessage(errorMessage);

        outDocument.globalItems.clear();
        outDocument.sections.clear();

        outDocument.globalItems.reserve(propertyTree.size());
        outDocument.sections.reserve(propertyTree.size());

        for (boost::property_tree::ptree::const_iterator rootIterator = propertyTree.begin(); rootIterator != propertyTree.end(); ++rootIterator)
        {
            const std::string& entryName = rootIterator->first;
            const boost::property_tree::ptree& entryNode = rootIterator->second;

            if (entryNode.empty())
            {
                GB_IniItem newItem;
                newItem.key = entryName;
                newItem.value = entryNode.data();
                outDocument.globalItems.push_back(std::move(newItem));
                continue;
            }

            if (!entryNode.data().empty())
            {
                SetErrorMessage(errorMessage, "Failed to convert INI data: a root entry contains both child nodes and a value.");
                return false;
            }

            GB_IniSection newSection;
            newSection.sectionName = entryName;
            newSection.items.reserve(entryNode.size());

            for (boost::property_tree::ptree::const_iterator itemIterator = entryNode.begin(); itemIterator != entryNode.end(); ++itemIterator)
            {
                const std::string& itemName = itemIterator->first;
                const boost::property_tree::ptree& itemNode = itemIterator->second;

                if (!itemNode.empty())
                {
                    SetErrorMessage(errorMessage, "Failed to convert INI data: section [" + entryName + "] contains nested child nodes.");
                    return false;
                }

                GB_IniItem newItem;
                newItem.key = itemName;
                newItem.value = itemNode.data();
                newSection.items.push_back(std::move(newItem));
            }

            outDocument.sections.push_back(std::move(newSection));
        }

        return true;
    }

    void BuildIniLineNumberCache(const std::string& iniText, IniLineNumberCache& outCache)
    {
        outCache.globalItems.clear();
        outCache.sections.clear();

        std::size_t currentSectionIndex = static_cast<std::size_t>(-1);
        std::size_t lineStartIndex = 0;
        long long lineNumber = 1;

        while (lineStartIndex < iniText.size())
        {
            std::size_t lineEndIndex = lineStartIndex;
            while (lineEndIndex < iniText.size() && iniText[lineEndIndex] != '\r' && iniText[lineEndIndex] != '\n')
            {
                lineEndIndex++;
            }

            const std::string_view rawLine(iniText.data() + lineStartIndex, lineEndIndex - lineStartIndex);
            const std::string_view trimmedLine = TrimIniTextView(rawLine);
            if (!trimmedLine.empty())
            {
                const char firstCharacter = trimmedLine[0];
                if (firstCharacter != ';' && firstCharacter != '#')
                {
                    std::string parsedSectionName;
                    if (TryParseIniSectionHeader(trimmedLine, parsedSectionName))
                    {
                        IniLineNumberSectionInfo newSectionInfo;
                        newSectionInfo.sectionName = std::move(parsedSectionName);
                        newSectionInfo.lineNumber = lineNumber;
                        outCache.sections.push_back(std::move(newSectionInfo));
                        currentSectionIndex = outCache.sections.size() - 1;
                    }
                    else
                    {
                        std::string parsedKey;
                        if (TryParseIniKeyName(trimmedLine, parsedKey))
                        {
                            IniLineNumberItemInfo newItemInfo;
                            newItemInfo.key = std::move(parsedKey);
                            newItemInfo.lineNumber = lineNumber;

                            if (currentSectionIndex == static_cast<std::size_t>(-1))
                            {
                                outCache.globalItems.push_back(std::move(newItemInfo));
                            }
                            else
                            {
                                outCache.sections[currentSectionIndex].items.push_back(std::move(newItemInfo));
                            }
                        }
                    }
                }
            }

            if (lineEndIndex >= iniText.size())
            {
                break;
            }

            if (iniText[lineEndIndex] == '\r'
                && lineEndIndex + 1 < iniText.size()
                && iniText[lineEndIndex + 1] == '\n')
            {
                lineStartIndex = lineEndIndex + 2;
            }
            else
            {
                lineStartIndex = lineEndIndex + 1;
            }

            lineNumber++;
        }
    }

    const IniLineNumberItemInfo* FindNextIniItemLineInfo(const std::vector<IniLineNumberItemInfo>& itemInfos,
        std::size_t& nextIndex,
        const std::string& key)
    {
        while (nextIndex < itemInfos.size())
        {
            const IniLineNumberItemInfo& currentItemInfo = itemInfos[nextIndex];
            nextIndex++;
            if (currentItemInfo.key == key)
            {
                return &currentItemInfo;
            }
        }

        return nullptr;
    }

    const IniLineNumberSectionInfo* FindNextIniSectionLineInfo(const std::vector<IniLineNumberSectionInfo>& sectionInfos,
        std::size_t& nextIndex,
        const std::string& sectionName)
    {
        while (nextIndex < sectionInfos.size())
        {
            const IniLineNumberSectionInfo& currentSectionInfo = sectionInfos[nextIndex];
            nextIndex++;
            if (currentSectionInfo.sectionName == sectionName)
            {
                return &currentSectionInfo;
            }
        }

        return nullptr;
    }

    void ApplyIniLineNumbers(const IniLineNumberCache& lineNumberCache, GB_IniDocument& document)
    {
        std::size_t nextGlobalItemIndex = 0;
        for (std::size_t index = 0; index < document.globalItems.size(); index++)
        {
            GB_IniItem& currentItem = document.globalItems[index];
            const IniLineNumberItemInfo* itemInfo = FindNextIniItemLineInfo(lineNumberCache.globalItems,
                nextGlobalItemIndex,
                currentItem.key);
            if (itemInfo != nullptr)
            {
                currentItem.lineNumber = itemInfo->lineNumber;
            }
        }

        std::size_t nextSectionIndex = 0;
        for (std::size_t sectionIndex = 0; sectionIndex < document.sections.size(); sectionIndex++)
        {
            GB_IniSection& currentSection = document.sections[sectionIndex];
            const IniLineNumberSectionInfo* sectionInfo = FindNextIniSectionLineInfo(lineNumberCache.sections,
                nextSectionIndex,
                currentSection.sectionName);
            if (sectionInfo == nullptr)
            {
                continue;
            }

            currentSection.lineNumber = sectionInfo->lineNumber;

            std::size_t nextItemIndex = 0;
            for (std::size_t itemIndex = 0; itemIndex < currentSection.items.size(); itemIndex++)
            {
                GB_IniItem& currentItem = currentSection.items[itemIndex];
                const IniLineNumberItemInfo* itemInfo = FindNextIniItemLineInfo(sectionInfo->items,
                    nextItemIndex,
                    currentItem.key);
                if (itemInfo != nullptr)
                {
                    currentItem.lineNumber = itemInfo->lineNumber;
                }
            }
        }
    }

    bool ParseIniTextToDocument(const std::string& iniText, GB_IniDocument& outDocument, const GB_IniParserOptions& options, std::string* errorMessage)
    {
        ClearErrorMessage(errorMessage);

        GB_IniDocument newDocument;
        newDocument.hasUtf8Bom = HasUtf8Bom(iniText);

        const std::string* iniTextToParse = &iniText;
        std::string iniTextWithoutBom;
        if (newDocument.hasUtf8Bom)
        {
            if (!options.removeUtf8Bom)
            {
                return FailIniParse(newDocument,
                    errorMessage,
                    1,
                    1,
                    "Input INI text contains a UTF-8 BOM, but removeUtf8Bom is false. Boost.PropertyTree ini_parser cannot safely preserve a leading UTF-8 BOM. Please enable removeUtf8Bom.");
            }

            iniTextWithoutBom.assign(iniText.begin() + static_cast<std::ptrdiff_t>(kUtf8BomLength), iniText.end());
            iniTextToParse = &iniTextWithoutBom;
        }

        boost::property_tree::ptree propertyTree;
        try
        {
            std::istringstream iniStream(*iniTextToParse);
            if (options.useClassicLocale)
            {
                iniStream.imbue(std::locale::classic());
            }

            boost::property_tree::ini_parser::read_ini(iniStream, propertyTree);
        }
        catch (const boost::property_tree::ini_parser::ini_parser_error& exceptionObject)
        {
            const char* exceptionText = exceptionObject.what();
            const std::string message = exceptionText != nullptr && exceptionText[0] != '\0'
                ? std::string("Failed to parse INI text: ") + exceptionText
                : std::string("Failed to parse INI text.");
            return FailIniParse(newDocument, errorMessage, static_cast<long long>(exceptionObject.line()), -1, message);
        }
        catch (const std::exception& exceptionObject)
        {
            return FailIniParse(newDocument, errorMessage, -1, -1, std::string("Failed to parse INI text due to an unexpected exception: ") + exceptionObject.what());
        }
        catch (...)
        {
            return FailIniParse(newDocument, errorMessage, -1, -1, "Failed to parse INI text due to an unknown exception.");
        }

        if (!ConvertIniPropertyTreeToDocument(propertyTree, newDocument, errorMessage))
        {
            AddIniDiagnostic(newDocument, GB_IniDiagnostic::Level::Error, -1, -1, errorMessage != nullptr ? *errorMessage : "Failed to convert INI parse result.");
            return false;
        }

        if (options.recordLineNumbers)
        {
            IniLineNumberCache lineNumberCache;
            BuildIniLineNumberCache(*iniTextToParse, lineNumberCache);
            ApplyIniLineNumbers(lineNumberCache, newDocument);
        }

        outDocument = std::move(newDocument);
        ClearErrorMessage(errorMessage);
        return true;
    }
}

bool GB_IniSection::HasKey(const std::string& key, bool caseSensitive) const
{
    return GetItem(key, caseSensitive) != nullptr;
}

const GB_IniItem* GB_IniSection::GetItem(const std::string& key, bool caseSensitive) const
{
    for (std::size_t index = 0; index < items.size(); index++)
    {
        const GB_IniItem& currentItem = items[index];
        if (AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            return &currentItem;
        }
    }

    return nullptr;
}

GB_IniItem* GB_IniSection::GetItem(const std::string& key, bool caseSensitive)
{
    for (std::size_t index = 0; index < items.size(); index++)
    {
        GB_IniItem& currentItem = items[index];
        if (AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            return &currentItem;
        }
    }

    return nullptr;
}

std::vector<const GB_IniItem*> GB_IniSection::GetItems(const std::string& key, bool caseSensitive) const
{
    std::vector<const GB_IniItem*> matchedItems;
    matchedItems.reserve(items.size());

    for (std::size_t index = 0; index < items.size(); index++)
    {
        const GB_IniItem& currentItem = items[index];
        if (key.empty() || AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            matchedItems.push_back(&currentItem);
        }
    }

    return matchedItems;
}

std::vector<GB_IniItem*> GB_IniSection::GetItems(const std::string& key, bool caseSensitive)
{
    std::vector<GB_IniItem*> matchedItems;
    matchedItems.reserve(items.size());

    for (std::size_t index = 0; index < items.size(); index++)
    {
        GB_IniItem& currentItem = items[index];
        if (key.empty() || AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            matchedItems.push_back(&currentItem);
        }
    }

    return matchedItems;
}

bool GB_IniSection::TryGetValue(const std::string& key, std::string& outValue, bool caseSensitive) const
{
    const GB_IniItem* item = GetItem(key, caseSensitive);
    if (item == nullptr)
    {
        return false;
    }

    outValue = item->value;
    return true;
}

std::string GB_IniSection::GetValue(const std::string& key, bool caseSensitive) const
{
    std::string value;
    if (!TryGetValue(key, value, caseSensitive))
    {
        return "";
    }

    return value;
}

bool GB_IniDocument::HasSection(const std::string& sectionName, bool caseSensitive) const
{
    return GetSection(sectionName, caseSensitive) != nullptr;
}

const GB_IniSection* GB_IniDocument::GetSection(const std::string& sectionName, bool caseSensitive) const
{
    for (std::size_t index = 0; index < sections.size(); index++)
    {
        const GB_IniSection& currentSection = sections[index];
        if (AreIniNamesEqual(currentSection.sectionName, sectionName, caseSensitive))
        {
            return &currentSection;
        }
    }

    return nullptr;
}

GB_IniSection* GB_IniDocument::GetSection(const std::string& sectionName, bool caseSensitive)
{
    for (std::size_t index = 0; index < sections.size(); index++)
    {
        GB_IniSection& currentSection = sections[index];
        if (AreIniNamesEqual(currentSection.sectionName, sectionName, caseSensitive))
        {
            return &currentSection;
        }
    }

    return nullptr;
}

std::vector<const GB_IniSection*> GB_IniDocument::GetSections(const std::string& sectionName, bool caseSensitive) const
{
    std::vector<const GB_IniSection*> matchedSections;
    matchedSections.reserve(sections.size());

    for (std::size_t index = 0; index < sections.size(); index++)
    {
        const GB_IniSection& currentSection = sections[index];
        if (sectionName.empty() || AreIniNamesEqual(currentSection.sectionName, sectionName, caseSensitive))
        {
            matchedSections.push_back(&currentSection);
        }
    }

    return matchedSections;
}

std::vector<GB_IniSection*> GB_IniDocument::GetSections(const std::string& sectionName, bool caseSensitive)
{
    std::vector<GB_IniSection*> matchedSections;
    matchedSections.reserve(sections.size());

    for (std::size_t index = 0; index < sections.size(); index++)
    {
        GB_IniSection& currentSection = sections[index];
        if (sectionName.empty() || AreIniNamesEqual(currentSection.sectionName, sectionName, caseSensitive))
        {
            matchedSections.push_back(&currentSection);
        }
    }

    return matchedSections;
}

bool GB_IniDocument::HasGlobalKey(const std::string& key, bool caseSensitive) const
{
    return GetGlobalItem(key, caseSensitive) != nullptr;
}

const GB_IniItem* GB_IniDocument::GetGlobalItem(const std::string& key, bool caseSensitive) const
{
    for (std::size_t index = 0; index < globalItems.size(); index++)
    {
        const GB_IniItem& currentItem = globalItems[index];
        if (AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            return &currentItem;
        }
    }

    return nullptr;
}

GB_IniItem* GB_IniDocument::GetGlobalItem(const std::string& key, bool caseSensitive)
{
    for (std::size_t index = 0; index < globalItems.size(); index++)
    {
        GB_IniItem& currentItem = globalItems[index];
        if (AreIniNamesEqual(currentItem.key, key, caseSensitive))
        {
            return &currentItem;
        }
    }

    return nullptr;
}

bool GB_IniDocument::TryGetGlobalValue(const std::string& key, std::string& outValue, bool caseSensitive) const
{
    const GB_IniItem* item = GetGlobalItem(key, caseSensitive);
    if (item == nullptr)
    {
        return false;
    }

    outValue = item->value;
    return true;
}

std::string GB_IniDocument::GetGlobalValue(const std::string& key, bool caseSensitive) const
{
    std::string value;
    if (!TryGetGlobalValue(key, value, caseSensitive))
    {
        return "";
    }

    return value;
}

bool GB_IniDocument::TryGetValue(const std::string& sectionName, const std::string& key, std::string& outValue, bool caseSensitiveSectionNames, bool caseSensitiveKeyNames) const
{
    const GB_IniSection* section = GetSection(sectionName, caseSensitiveSectionNames);
    if (section == nullptr)
    {
        return false;
    }

    return section->TryGetValue(key, outValue, caseSensitiveKeyNames);
}

std::string GB_IniDocument::GetValue(const std::string& sectionName, const std::string& key, bool caseSensitiveSectionNames, bool caseSensitiveKeyNames) const
{
    std::string value;
    if (!TryGetValue(sectionName, key, value, caseSensitiveSectionNames, caseSensitiveKeyNames))
    {
        return "";
    }

    return value;
}

bool GB_IniParser::ParseToDocument(const std::string& iniText, GB_IniDocument& outDocument, const GB_IniParserOptions& options, std::string* errorMessage)
{
    return ParseIniTextToDocument(iniText, outDocument, options, errorMessage);
}

bool GB_IniParser::ParseFileToDocument(const std::string& iniFilePath, GB_IniDocument& outDocument, const GB_IniParserOptions& options, std::string* errorMessage)
{
    std::string iniText;
    if (!ReadTextFile(iniFilePath, iniText, errorMessage))
    {
        return false;
    }

    if (ParseToDocument(iniText, outDocument, options, errorMessage))
    {
        return true;
    }

    if (errorMessage != nullptr)
    {
        if (errorMessage->empty())
        {
            *errorMessage = "Failed to parse INI file: " + iniFilePath + ".";
        }
        else
        {
            *errorMessage = "Failed to parse INI file " + iniFilePath + ": " + *errorMessage;
        }
    }

    return false;
}

