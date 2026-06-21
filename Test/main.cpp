#include "GB_RunTests.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    bool systemProcessChildHandled = false;
    const int systemProcessChildExitCode = RunGB_SystemProcessChildMode(argc, argv, systemProcessChildHandled);
    if (systemProcessChildHandled)
    {
        return systemProcessChildExitCode;
    }

    if (argc == 2 && argv != nullptr && argv[1] != nullptr && std::string(argv[1]) == "--gb-system-clipboard-isolated-child")
    {
        return RunGB_SystemClipboardIsolatedTests();
    }
    if (argc == 2 && argv != nullptr && argv[1] != nullptr && std::string(argv[1]) == "--gb-system-file-watcher-only")
    {
        return RunGB_SystemFileWatcherTests();
    }
    if (argc == 2 && argv != nullptr && argv[1] != nullptr && std::string(argv[1]) == "--gb-system-session-only")
    {
        return RunGB_SystemSessionTests();
    }
    if (argc == 2 && argv != nullptr && argv[1] != nullptr && std::string(argv[1]) == "--gb-system-network-only")
    {
        return RunGB_SystemNetworkTests();
    }

    int failedCount = 0;
    failedCount += RunGB_Utf8StringTests();
    failedCount += RunGB_SystemDeviceTests();
    failedCount += RunGB_SystemFileWatcherTests();
    failedCount += RunGB_SystemClipboardTests();
    failedCount += RunGB_SystemWindowTests();
    failedCount += RunGB_SystemProcessTests();
    failedCount += RunGB_SystemSessionTests();
    failedCount += RunGB_SystemNetworkTests();

    if (failedCount != 0)
    {
        std::cerr << "Test failed. Failed test group count: " << failedCount << std::endl;
        return 1;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
