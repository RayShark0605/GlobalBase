#ifndef GLOBALBASE_PORT_H_H
#define GLOBALBASE_PORT_H_H

#ifdef GLOBALBASE_API
#define GLOBALBASE_PORT __declspec(dllexport)
#else
#define GLOBALBASE_PORT __declspec(dllimport)
#endif

#endif