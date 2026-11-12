// m20: the command line, from the platform
#include "args.h"

#include "m20/m20_platform.h"
#include "memory.h"

#include <string.h>

int get_command_line(char ***args, int *arg_count)
{
    *args = Memory_Alloc(M20_Argc * sizeof(char *));
    *arg_count = M20_Argc;
    for (int i = 0; i < M20_Argc; i++) {
        size_t size = strlen(M20_Argv[i]) + 1;
        (*args)[i] = Memory_Alloc(size);
        memcpy((*args)[i], M20_Argv[i], size);
    }
    return 1;
}
