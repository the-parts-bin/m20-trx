#include "specific/s_display.h"

#include "global/vars.h"
#include "specific/s_hwr.h"

void S_NoFade()
{
    // not implemented in TombATI
}

void S_FadeInInventory(int32_t fade)
{
    if (CurrentLevel != GF.title_level_num) {
        HWR_CopyPicture();
    }
}

void S_FadeOutInventory(int32_t fade)
{
    // not implemented in TombATI
}

void S_CopyBufferToScreen()
{
    // m20: the background picture into the frame
    HWR_ShowPicture();
}
