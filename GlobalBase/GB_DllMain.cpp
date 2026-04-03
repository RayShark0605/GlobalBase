#include <windows.h>
#include "GB_DelayLoadRuntime.h"

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        GB_SetSelfModuleHandle(moduleHandle);
        DisableThreadLibraryCalls(moduleHandle);
    }

    return TRUE;
}