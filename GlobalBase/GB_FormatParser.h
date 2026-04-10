#ifndef GLOBALBASE_FORMAT_PARSER_H_H
#define GLOBALBASE_FORMAT_PARSER_H_H

#include "GlobalBasePort.h"
#include "GB_BaseTypes.h"
#include "GB_Variant.h"
#include <string>

class GLOBALBASE_PORT GB_JsonParser
{
public:
	static bool ParseToVariant(const std::string& jsonText, GB_Variant& outValue, std::string* errorMessage = nullptr);
	static bool ParseToVariantMap(const std::string& jsonText, GB_VariantMap& outMap, std::string* errorMessage = nullptr);
};





























#endif