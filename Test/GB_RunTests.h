#pragma once

int RunGB_Utf8StringTests();
int RunGB_SystemDeviceTests();
int RunGB_SystemFileWatcherTests();
int RunGB_SystemClipboardTests();
int RunGB_SystemClipboardIsolatedTests();
int RunGB_SystemWindowTests();
int RunGB_SystemProcessTests();
int RunGB_SystemProcessChildMode(int argc, char* argv[], bool& handled);
