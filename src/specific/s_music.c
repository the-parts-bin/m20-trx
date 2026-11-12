// m20: no CD audio (the music tracks aren't in the levels)
#include "specific/s_music.h"

bool S_Music_Init()
{
    return false;
}

bool S_Music_Play(int16_t track)
{
    return false;
}

bool S_Music_Stop()
{
    return true;
}

bool S_Music_SetVolume(int16_t volume)
{
    return true;
}

bool S_Music_Pause()
{
    return true;
}

bool S_Music_Unpause()
{
    return true;
}
