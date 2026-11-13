#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void Log_Init()
{
#ifndef M20_BOARD
    freopen("./Tomb1Main.log", "w", stdout);   // (on the board: the UART)
#endif
}

void Log_Message(
    const char *file, int line, const char *func, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    printf("%s %d %s ", file, line, func);
    vprintf(fmt, va);
    printf("\n");
    va_end(va);
    fflush(stdout);
}
