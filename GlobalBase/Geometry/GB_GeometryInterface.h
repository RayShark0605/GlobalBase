#ifndef GLOBALBASE_GEOMETRY_INTERFACE_H_H
#define GLOBALBASE_GEOMETRY_INTERFACE_H_H

#include "../GlobalBasePort.h"
#include "../GB_BaseTypes.h"
#include <string>

uint64_t GB_GenerateClassTypeId(const std::string& classType);

class GLOBALBASE_PORT GB_SerializableClass
{
public:
	virtual ~GB_SerializableClass() = default;

	// 获取类类型标识字符串（每个派生类应返回唯一且固定的字符串）
	virtual const std::string& GetClassType() const = 0;

	// 获取类类型标识 Id
	virtual uint64_t GetClassTypeId() const = 0;

	// 序列化
	virtual std::string SerializeToString() const = 0;
	virtual GB_ByteBuffer SerializeToBinary() const = 0;

	// 反序列化
	virtual bool Deserialize(const std::string& data) = 0;
	virtual bool Deserialize(const GB_ByteBuffer& data) = 0;
};

#define GB_GetClassType(ClassName) ClassName().GetClassType()
#define GB_GetClassTypeId(ClassName) ClassName().GetClassTypeId()



#endif