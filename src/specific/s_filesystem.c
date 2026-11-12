// m20: paths are relative to the current directory (the game's)
#include "specific/s_filesystem.h"

size_t S_File_GetMaxPath()
{
    return 256;
}

bool S_File_IsRelative(const char *path)
{
    return path[0] != '/' && path[0] != '\\';
}

const char *S_File_GetGameDirectory()
{
    return NULL;
}
