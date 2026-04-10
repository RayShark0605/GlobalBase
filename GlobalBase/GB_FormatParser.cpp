#include "GB_FormatParser.h"

#include "cpl_error.h"
#include "cpl_json.h"

#include <exception>
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
