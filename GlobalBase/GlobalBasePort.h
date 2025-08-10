#ifndef GLOBALBASE_PORT_H_H
#define GLOBALBASE_PORT_H_H

#if defined(GLOBALBASE_STATIC)
#define GLOBALBASE_PORT

// Windows: 用 __declspec(dllexport/dllimport)
#elif defined(_WIN32) || defined(_WIN64)
#if defined(GLOBALBASE_EXPORTS)
#define GLOBALBASE_PORT __declspec(dllexport)
#else
#define GLOBALBASE_PORT __declspec(dllimport)
#endif

// 非 Windows: 用 ELF 的可见性属性，或留空
#else
#if defined(__GNUC__) || defined(__clang__)
#define GLOBALBASE_PORT __attribute__((visibility("default")))
#else
#define GLOBALBASE_PORT
#endif
#endif

#endif