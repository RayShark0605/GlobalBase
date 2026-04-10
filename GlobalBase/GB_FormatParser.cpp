#include "GB_FormatParser.h"
#include "cpl_json.h"

namespace
{
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

                newMap[child.GetName()] = std::move(childValue);
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
}

bool GB_JsonParser::ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage)
{
    if (jsonText.empty())
    {
        if (errorMessage)
        {
			*errorMessage = "Input JSON text is empty.";
        }
        return false;
    }

    CPLJSONDocument jsonDocument;
    if (!jsonDocument.LoadMemory(jsonText))
    {
        if (errorMessage)
        {
			*errorMessage = "Failed to parse JSON text. Please ensure the input is valid JSON.";
        }
        return false;
    }

    GB_Variant newValue;
    if (!ConvertJsonNodeToVariant(jsonDocument.GetRoot(), newValue))
    {
        if (errorMessage)
        {
			*errorMessage = "Failed to convert JSON structure to GB_Variant. The JSON may contain unsupported types or structures.";
        }
        return false;
    }

    outValue = std::move(newValue);
    return true;
}

