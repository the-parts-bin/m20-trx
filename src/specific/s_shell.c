// m20: the shell, over src/m20/m20_platform.h (the window, the message loop
// and DirectDraw's start and end are the platform's)
#include "specific/s_shell.h"

#include "game/gamebuf.h"
#include "game/input.h"
#include "game/random.h"
#include "global/vars.h"
#include "log.h"
#include "m20/m20_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void S_Shell_SeedRandom()
{
    uint32_t t = M20_TimeUs();
#ifndef M20_BOARD
    if (getenv("M20_SEED")) {   // (the harness: the same game on every run)
        t = (uint32_t)strtoul(getenv("M20_SEED"), NULL, 0);
    }
#endif
    Random_SeedControl(t & 0xFFFF);
    Random_SeedDraw((t >> 16) ^ 0x1234);
}

void S_Shell_SpinMessageLoop()
{
    M20_PollInput();
}

void S_Shell_ExitSystem(const char *message)
{
    LOG_ERROR("%s", message);
    M20_Fatal(message);
}

void S_Shell_ExitSystemFmt(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char message[150];
    vsnprintf(message, 150, fmt, va);
    va_end(va);
    S_Shell_ExitSystem(message);
}
