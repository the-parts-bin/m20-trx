#include "filesystem.h"

#include "log.h"
#include "memory.h"
#include "specific/s_shell.h"
#include "specific/s_filesystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MYFILE {
    FILE *fp;
};

bool File_IsRelative(const char *path)
{
    return S_File_IsRelative(path);
}

const char *File_GetGameDirectory()
{
    return S_File_GetGameDirectory();
}

bool File_Exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fclose(fp);
        return true;
    }
    return false;
}

// m20: the game's paths have backslashes, and the files on the CD are upper
// case: '/' for '\\', and if the path as written isn't there, its upper case
void File_GetFullPath(const char *path, char **out)
{
    size_t n = strlen(path);
    char *p = Memory_Alloc(n + 1);
    for (size_t i = 0; i <= n; i++) {
        p[i] = path[i] == '\\' ? '/' : path[i];
    }
    if (!File_Exists(p)) {
        char *u = Memory_Alloc(n + 1);
        for (size_t i = 0; i <= n; i++) {
            u[i] = (p[i] >= 'a' && p[i] <= 'z') ? p[i] - 32 : p[i];
        }
        if (File_Exists(u)) {
            Memory_Free(p);
            p = u;
        } else {
            Memory_Free(u);
        }
    }
    *out = p;
}

MYFILE *File_Open(const char *path, FILE_OPEN_MODE mode)
{
    char *full_path = NULL;
    File_GetFullPath(path, &full_path);
    MYFILE *file = Memory_Alloc(sizeof(MYFILE));
    switch (mode) {
    case FILE_OPEN_WRITE:
        file->fp = fopen(full_path, "wb");
        break;
    case FILE_OPEN_READ:
        file->fp = fopen(full_path, "rb");
        break;
    default:
        file->fp = NULL;
        break;
    }
    if (full_path) {
        Memory_Free(full_path);
    }
    if (!file->fp) {
        Memory_Free(file);
        return NULL;
    }
    return file;
}

size_t File_Read(void *data, size_t item_size, size_t count, MYFILE *file)
{
    return fread(data, item_size, count, file->fp);
}

size_t File_Write(
    const void *data, size_t item_size, size_t count, MYFILE *file)
{
    return fwrite(data, item_size, count, file->fp);
}

void File_Seek(MYFILE *file, size_t pos, FILE_SEEK_MODE mode)
{
    switch (mode) {
    case FILE_SEEK_SET:
        fseek(file->fp, pos, SEEK_SET);
        break;
    case FILE_SEEK_CUR:
        fseek(file->fp, pos, SEEK_CUR);
        break;
    case FILE_SEEK_END:
        fseek(file->fp, pos, SEEK_END);
        break;
    }
}

size_t File_Size(MYFILE *file)
{
    size_t old = ftell(file->fp);
    fseek(file->fp, 0, SEEK_END);
    size_t size = ftell(file->fp);
    fseek(file->fp, old, SEEK_SET);
    return size;
}

void File_Close(MYFILE *file)
{
    fclose(file->fp);
    Memory_Free(file);
}

int File_Delete(const char *path)
{
    return remove(path);
}

bool File_Load(const char *path, char **output_data, size_t *output_size)
{
    MYFILE *fp = File_Open(path, FILE_OPEN_READ);
    if (!fp) {
        LOG_ERROR("Can't open file %s", path);
        return false;
    }

    size_t data_size = File_Size(fp);
    char *data = Memory_Alloc(data_size);
    if (File_Read(data, sizeof(char), data_size, fp) != data_size) {
        LOG_ERROR("Can't read file %s", path);
        Memory_Free(data);
        return false;
    }
    File_Close(fp);

    *output_data = data;
    *output_size = data_size;
    return true;
}
