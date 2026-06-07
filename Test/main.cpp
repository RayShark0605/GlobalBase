#include "GB_RunTests.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc == 2 && argv != nullptr && argv[1] != nullptr && std::string(argv[1]) == "--gb-system-clipboard-isolated-child")
    {
        return RunGB_SystemClipboardIsolatedTests();
    }

    int failedCount = 0;
    failedCount += RunGB_Utf8StringTests();
    failedCount += RunGB_SystemDeviceTests();
    failedCount += RunGB_SystemClipboardTests();
    failedCount += RunGB_SystemWindowTests();

    if (failedCount != 0)
    {
        std::cerr << "Test failed. Failed test group count: " << failedCount << std::endl;
        return 1;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
