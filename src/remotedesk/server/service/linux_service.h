#pragma once

class LinuxService
{
public:
    static int run(int argc, char* argv[]);
    static int install(int argc, char* argv[]);
    static int uninstall(int argc, char* argv[]);
};
