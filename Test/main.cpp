#include "GB_RunTests.h"

#include <iostream>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    int failedCount = 0;
    failedCount += RunGB_Utf8StringTests();
    failedCount += RunGB_SystemDeviceTests();

    if (failedCount != 0)
    {
        std::cerr << "Test failed. Failed test group count: " << failedCount << std::endl;
        return 1;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
