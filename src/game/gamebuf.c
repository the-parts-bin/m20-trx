#include "game/gamebuf.h"

#include "log.h"
#include "memory.h"

#include <string.h>
#include "specific/s_shell.h"

// m20: the game's memory, from the build (the Makefile): the original's 16 MB
// is TombATI's, far more than a level uses
#ifdef M20_GAMEBUF
#define MALLOC_SIZE M20_GAMEBUF
#else
#define MALLOC_SIZE 0x1000000 // 16 MB
#endif

static char *m_GameMemoryPointer = NULL;
static char *m_GameAllocMemPointer = NULL;
static size_t m_GameAllocMemFree = 0;
static size_t m_TypeBytes[64];   // m20: what each buffer type took

static const char *GameBuf_GetBufferName(GAME_BUFFER buffer)
{
    // clang-format off
    switch (buffer) {
        case GBUF_TEXTURE_PAGES:            return "Texture Pages";
        case GBUF_MESH_POINTERS:            return "Mesh Pointers";
        case GBUF_MESHES:                   return "Meshes";
        case GBUF_ANIMS:                    return "Animations";
        case GBUF_ANIM_CHANGES:             return "Animation changes";
        case GBUF_ANIM_RANGES:              return "Animation ranges";
        case GBUF_ANIM_COMMANDS:            return "Animation commands";
        case GBUF_ANIM_BONES:               return "Animation bones";
        case GBUF_ANIM_FRAMES:              return "Animation frames";
        case GBUF_ROOM_INFOS:               return "Room information";
        case GBUF_ROOM_MESH:                return "Room meshes";
        case GBUF_ROOM_DOOR:                return "Room doors";
        case GBUF_ROOM_FLOOR:               return "Room floor information";
        case GBUF_ROOM_LIGHTS:              return "Room lights";
        case GBUF_ROOM_STATIC_MESH_INFOS:   return "Room static meshes";
        case GBUF_FLOOR_DATA:               return "Floor data";
        case GBUF_ITEMS:                    return "Items";
        case GBUF_CAMERAS:                  return "Cameras";
        case GBUF_SOUND_FX:                 return "Sound effects";
        case GBUF_BOXES:                    return "Boxes";
        case GBUF_GROUNDZONE:               return "GroundZone";
        case GBUF_FLYZONE:                  return "FlyZone";
        case GBUF_ANIMATING_TEXTURE_RANGES: return "Animating texture ranges";
        case GBUF_CINEMATIC_FRAMES:         return "Cinematic frames";
        case GBUF_LOADDEMO_BUFFER:          return "Load demo buffer";
        case GBUF_EXTRA_DOOR_STUFF:         return "Room doors (2)";
        case GBUF_EFFECTS:                  return "Effects";
        case GBUF_CREATURE_INFO:            return "Creature information";
        case GBUF_CREATURE_LOT:             return "Creature pathfinding";
        case GBUF_SAMPLE_INFOS:             return "Sample information";
        case GBUF_SAMPLES:                  return "Samples";
        case GBUF_TRAP_DATA:                return "Trap data";
        case GBUF_CREATURE_DATA:            return "Creature data";
    }
    // clang-format on
    return "Unknown";
};

void GameBuf_Init()
{
    // m20: allocated once, reset at each level's load (it was allocated
    // again at every level, and the last one's never freed)
    if (!m_GameMemoryPointer) {
        m_GameMemoryPointer = Memory_Alloc(MALLOC_SIZE);
    }
    m_GameAllocMemPointer = m_GameMemoryPointer;
    m_GameAllocMemFree = MALLOC_SIZE;
    memset(m_TypeBytes, 0, sizeof(m_TypeBytes));
}

// m20: what each buffer type took, to the log
void GameBuf_LogUse()
{
    for (int i = 0; i < 64; i++) {
        if (m_TypeBytes[i]) {
            LOG_INFO(
                "m20: %-28s %7d", GameBuf_GetBufferName(i), (int)m_TypeBytes[i]);
        }
    }
}

// m20: what the level loaded uses
size_t GameBuf_GetUsed()
{
    return MALLOC_SIZE - m_GameAllocMemFree;
}

size_t GameBuf_GetSize()
{
    return MALLOC_SIZE;
}

void GameBuf_Shutdown()
{
    Memory_Free(m_GameMemoryPointer);
    m_GameMemoryPointer = NULL;
    m_GameAllocMemPointer = NULL;
    m_GameAllocMemFree = 0;
}

void *GameBuf_Alloc(int32_t alloc_size, GAME_BUFFER buffer)
{
    size_t aligned_size = (alloc_size + 3) & ~3;

    if (aligned_size > m_GameAllocMemFree) {
        S_Shell_ExitSystemFmt(
            "GameBuf_Alloc(): OUT OF MEMORY %s %d",
            GameBuf_GetBufferName(buffer), aligned_size);
    }

    m_TypeBytes[buffer & 63] += aligned_size;
    void *result = m_GameAllocMemPointer;
    m_GameAllocMemFree -= aligned_size;
    m_GameAllocMemPointer += aligned_size;
    return result;
}
