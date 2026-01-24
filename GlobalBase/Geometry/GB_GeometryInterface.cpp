#include "GB_GeometryInterface.h"
#include <cstdint>
#include <cstring>
#include <limits>

static inline uint64_t ComputeFnv1a64(const char* bytes, size_t length)
{
    constexpr static uint64_t offsetBasis = 14695981039346656037ull;
    constexpr static uint64_t prime = 1099511628211ull;

    uint64_t hash = offsetBasis;
    for (size_t i = 0; i < length; i++)
    {
        hash ^= static_cast<unsigned char>(bytes[i]);
        hash *= prime;
    }
    return hash;
}

uint64_t GB_GenerateClassTypeId(const std::string& classType)
{
    if (classType.empty())
    {
		return std::numeric_limits<uint64_t>::max();
    }
    return ComputeFnv1a64(classType.data(), classType.size());
}
