#pragma once
#include "GlobalBasePort.h"
#include <windows.h>

/**
 * @brief 初始化延迟加载运行时环境。
 *
 * 该函数会主动尝试加载当前模块依赖的延迟加载三方 DLL，
 * 加载顺序为：
 * 1. 当前模块所在目录下的 GlobalBaseDependencies 子目录；
 * 2. 当前模块所在目录。
 *
 * 如果任意关键依赖加载失败，会弹出一次 MessageBox 进行告警。
 *
 * @return true  所有关键依赖均已成功准备就绪。
 * @return false 至少存在一个关键依赖未成功准备就绪。
 */
GLOBALBASE_PORT bool GB_InitializeRuntime();

/**
 * @brief 供 DllMain 在 DLL_PROCESS_ATTACH 时记录当前模块句柄。
 *
 * @param moduleHandle 当前模块的模块句柄。
 */
GLOBALBASE_PORT void GB_SetSelfModuleHandle(HMODULE moduleHandle);
