#include "service.h"
#include "linux_service.h"   // from server/linux_service/

#include <cstdio>
#include <cstring>

int platformMain(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--install") == 0) {
            LinuxService::printInstallInstructions();
            return 0;
        }
        if (strcmp(argv[i], "--uninstall") == 0) {
            LinuxService::printUninstallInstructions();
            return 0;
        }
        if (strcmp(argv[i], "--service") == 0) {
            return LinuxService::run(argc, argv);
        }
    }
    return -1;
}
