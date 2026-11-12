// m20: the clock, from the platform's microseconds
#include "specific/s_clock.h"

#include "global/vars.h"
#include "m20/m20_platform.h"

#define US_PER_TICK (1000000 / TICKS_PER_SECOND)

static uint32_t m_Last;
static uint32_t m_Frac;     // microseconds not yet counted as ticks

bool S_Clock_Init()
{
    m_Last = M20_TimeUs();
    m_Frac = 0;
    return true;
}

int32_t S_Clock_GetMS()
{
    return M20_TimeUs() / 1000;
}

extern bool M20_Scripted;

int32_t S_Clock_Sync()
{
    if (M20_Scripted) {
        return TICKS_PER_FRAME;   // the harness's scripted runs: one frame a frame
    }
    uint32_t now = M20_TimeUs();
    m_Frac += now - m_Last;
    m_Last = now;
    int32_t ticks = m_Frac / US_PER_TICK;
    m_Frac -= ticks * US_PER_TICK;
    return ticks;
}

int32_t S_Clock_SyncTicks(int32_t target)
{
    if (M20_Scripted) {
        return target;
    }
    int32_t elapsed = 0;
    do {
        elapsed += S_Clock_Sync();
        if (elapsed < target) {
            M20_PollInput();
        }
    } while (elapsed < target);
    return elapsed;
}
