#pragma once

class LinuxService
{
public:
    static int run(int argc, char* argv[]);
    static void printInstallInstructions();
    static void printUninstallInstructions();
};
