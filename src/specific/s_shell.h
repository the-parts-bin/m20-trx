#ifndef T1M_SPECIFIC_S_SHELL_H
#define T1M_SPECIFIC_S_SHELL_H

#include <stdint.h>

void S_Shell_SeedRandom();
void S_Shell_ExitSystem(const char *message);
void S_Shell_ExitSystemFmt(const char *fmt, ...);
void S_Shell_SpinMessageLoop();
// m20: the heap's use and peak (the platform's)
void S_Shell_ReportMemory(const char *what);

#endif
