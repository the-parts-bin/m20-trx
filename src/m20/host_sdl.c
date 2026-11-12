// The test harness's platform (src/m20/m20_platform.h): 32-bit x86 Linux
// with SDL2, the board's frame size and heap.
//
// Window: the frame scaled to 1280 x 720, as the board shows it. F12 saves
// the frame as fNNNNN.ppm. The heap is an arena the size of the board's
// (M20_HEAP bytes, the Makefile), every malloc of the game's from it, so
// what doesn't fit on the board doesn't fit here either; its use and peak
// are printed at each level load (S_Shell_ReportMemory) and at exit.
//
// Environment: M20_STEPS=N quits after N frames (with M20_SHOTS=a,b,c the
// frames saved as images, and M20_KEYS=frame:hex,... the keys held from
// those frames on, as bits of the INPUT_STATE: no window then, for scripted
// runs); M20_HEADLESS=1 no window; M20_LEVEL=N starts at level N (the
// gameflow's numbers: 0 Lara's Home, 1 Caves, ... 15 the Great Pyramid).
#include "m20/m20_platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

uint16_t *M20_Frame;
uint8_t M20_Keys[256];
int M20_Argc;
char **M20_Argv;

static SDL_Window *m_Window;
static SDL_Renderer *m_Renderer;
static SDL_Texture *m_Texture;
static bool m_Headless;
static int m_FrameNo;
static int m_Steps = -1;
static char m_Shots[4096];
static const char *m_KeyScript;
int M20_StartLevel = -1;     // M20_LEVEL: start at this level
int M20_CamTest;            // M20_CAMTEST=1: the camera at random (game/camera.c)
uint32_t M20_ScriptKeys;     // read by s_input.c in scripted runs
bool M20_Scripted;

// ---------------------------------------------------------------------
// The heap: first fit over a fixed arena, 8-byte headers, coalescing
// ---------------------------------------------------------------------
#ifndef M20_HEAP
#define M20_HEAP (5u * 1024 * 1024)
#endif
typedef struct BLOCK {
    uint32_t size;      // bytes, header included; bit 0: in use
    uint32_t prev;      // the previous block's size (0: first)
} BLOCK;
static uint8_t m_Arena[M20_HEAP] __attribute__((aligned(16)));
static bool m_ArenaReady;
static size_t m_HeapUse, m_HeapPeak;

static void heap_init(void)
{
    BLOCK *b = (BLOCK *)m_Arena;
    b->size = (M20_HEAP & ~7u);
    b->prev = 0;
    m_ArenaReady = true;
}

static BLOCK *heap_next(BLOCK *b)
{
    uint8_t *n = (uint8_t *)b + (b->size & ~7u);
    return n < m_Arena + (M20_HEAP & ~7u) ? (BLOCK *)n : NULL;
}

void *__wrap_malloc(size_t size)
{
    if (!m_ArenaReady) {
        heap_init();
    }
    uint32_t need = (uint32_t)((size + sizeof(BLOCK) + 7) & ~7u);
    for (BLOCK *b = (BLOCK *)m_Arena; b; b = heap_next(b)) {
        if ((b->size & 1) || b->size < need) {
            continue;
        }
        if (b->size - need >= 64) {
            BLOCK *rest = (BLOCK *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->prev = need;
            BLOCK *after = heap_next(rest);
            if (after) {
                after->prev = rest->size;
            }
            b->size = need;
        }
        b->size |= 1;
        m_HeapUse += b->size & ~7u;
        if (m_HeapUse > m_HeapPeak) {
            m_HeapPeak = m_HeapUse;
        }
        void *p = b + 1;
        memset(p, 0xA5, (b->size & ~7u) - sizeof(BLOCK));
        return p;
    }
    fprintf(
        stderr, "OUT OF MEMORY: malloc(%u) with %u of %u in use\n",
        (unsigned)size, (unsigned)m_HeapUse, (unsigned)M20_HEAP);
    return NULL;
}

void __wrap_free(void *p)
{
    if (!p) {
        return;
    }
    BLOCK *b = (BLOCK *)p - 1;
    b->size &= ~1u;
    m_HeapUse -= b->size;
    BLOCK *n = heap_next(b);
    if (n && !(n->size & 1)) {
        b->size += n->size;
        BLOCK *nn = heap_next(b);
        if (nn) {
            nn->prev = b->size;
        }
    }
    if (b->prev) {
        BLOCK *pb = (BLOCK *)((uint8_t *)b - b->prev);
        if (!(pb->size & 1)) {
            pb->size += b->size;
            BLOCK *nn = heap_next(pb);
            if (nn) {
                nn->prev = pb->size;
            }
        }
    }
}

void *__wrap_calloc(size_t n, size_t size)
{
    void *p = __wrap_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void *__wrap_realloc(void *p, size_t size)
{
    if (!p) {
        return __wrap_malloc(size);
    }
    BLOCK *b = (BLOCK *)p - 1;
    size_t have = (b->size & ~7u) - sizeof(BLOCK);
    if (have >= size) {
        return p;
    }
    void *q = __wrap_malloc(size);
    if (q) {
        memcpy(q, p, have);
        __wrap_free(p);
    }
    return q;
}

typedef struct {
    int32_t prims, verts, prims_peak, verts_peak, overflows;
} HWR_STATS_COPY;
extern HWR_STATS_COPY HWR_Stats;   // specific/s_hwr.h

void S_Shell_ReportMemory(const char *what)
{
    fprintf(
        stderr, "render: peak %d primitives, %d vertices, %d dropped\n",
        HWR_Stats.prims_peak, HWR_Stats.verts_peak, HWR_Stats.overflows);
    fprintf(
        stderr, "heap: %s: %u KB in use, peak %u KB, of %u KB\n", what,
        (unsigned)(m_HeapUse >> 10), (unsigned)(m_HeapPeak >> 10),
        (unsigned)(M20_HEAP >> 10));
}

// ---------------------------------------------------------------------
// Video, input, time
// ---------------------------------------------------------------------
static void save_ppm(void)
{
    char name[32];
    snprintf(name, sizeof(name), "f%05d.ppm", m_FrameNo);
    FILE *f = fopen(name, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", M20_W, M20_H);
    for (int i = 0; i < M20_W * M20_H; i++) {
        uint16_t c = M20_Frame[i];
        uint8_t rgb[3] = { (uint8_t)((c >> 11) << 3),
                           (uint8_t)(((c >> 5) & 63) << 2),
                           (uint8_t)((c & 31) << 3) };
        fwrite(rgb, 3, 1, f);
    }
    fclose(f);
    fprintf(stderr, "saved %s\n", name);
}

static bool listed(const char *list, int n)
{
    for (const char *p = list; *p;) {
        if (atoi(p) == n) {
            return true;
        }
        p = strchr(p, ',');
        if (!p) {
            break;
        }
        p++;
    }
    return false;
}

static void script_keys(void)
{
    // frame:hex pairs, in order; the last one at or before this frame holds
    for (const char *p = m_KeyScript; p && *p;) {
        int f = atoi(p);
        const char *c = strchr(p, ':');
        if (!c || f > m_FrameNo) {
            break;
        }
        M20_ScriptKeys = strtoul(c + 1, NULL, 16);
        p = strchr(c, ',');
        if (!p) {
            break;
        }
        p++;
    }
}

void M20_Present(void)
{
    if (m_Steps >= 0) {
        if (listed(m_Shots, m_FrameNo)) {
            save_ppm();
        }
        if (m_FrameNo >= m_Steps) {
            S_Shell_ReportMemory("exit");
            exit(0);
        }
    }
    if (!m_Headless) {
        SDL_UpdateTexture(m_Texture, NULL, M20_Frame, M20_W * 2);
        SDL_RenderClear(m_Renderer);
        SDL_RenderCopy(m_Renderer, m_Texture, NULL, NULL);
        SDL_RenderPresent(m_Renderer);
    }
    m_FrameNo++;
    if (M20_Scripted) {
        script_keys();
    }
}

void M20_PollInput(void)
{
    if (m_Headless) {
        return;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            S_Shell_ReportMemory("exit");
            exit(0);
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12
            && !e.key.repeat) {
            save_ppm();
        }
    }
    int n;
    const uint8_t *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < 256; i++) {
        M20_Keys[i] = i < n ? k[i] : 0;
    }
}

uint32_t M20_TimeUs(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000000ull + tv.tv_usec);
}

void M20_Fatal(const char *message)
{
    fprintf(stderr, "FATAL: %s\n", message);
    S_Shell_ReportMemory("fatal");
    exit(1);
}

// ---------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------
static SDL_AudioDeviceID m_AudioDev;   // 0: no sound

static void audio_callback(void *user, Uint8 *stream, int len)
{
    S_Sound_Mix((int16_t *)stream, len / 4);
}

void M20_AudioInit(void)
{
    if (m_Headless || getenv("M20_NOSOUND")) {
        return;
    }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = M20_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    m_AudioDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (m_AudioDev) {
        SDL_PauseAudioDevice(m_AudioDev, 0);
    } else {
        fprintf(stderr, "no audio: %s\n", SDL_GetError());
    }
}

void M20_AudioLock(bool lock)
{
    // the device's own lock (SDL_LockAudio locks the legacy device 1,
    // which SDL_OpenAudioDevice never returns)
    if (!m_AudioDev) {
        return;
    }
    if (lock) {
        SDL_LockAudioDevice(m_AudioDev);
    } else {
        SDL_UnlockAudioDevice(m_AudioDev);
    }
}

// ---------------------------------------------------------------------
void Shell_Main(void);

int main(int argc, char **argv)
{
    M20_Argc = argc;
    M20_Argv = argv;
    static uint16_t frame[M20_W * M20_H];
    M20_Frame = frame;

    const char *s = getenv("M20_STEPS");
    if (s) {
        m_Steps = atoi(s);
        m_Headless = true;
        M20_Scripted = true;
        const char *shots = getenv("M20_SHOTS");
        if (shots) {
            snprintf(m_Shots, sizeof(m_Shots), "%s", shots);
        }
        m_KeyScript = getenv("M20_KEYS");
        script_keys();
    }
    if (getenv("M20_LEVEL")) {
        M20_StartLevel = atoi(getenv("M20_LEVEL"));
    }
    if (getenv("M20_CAMTEST")) {
        M20_CamTest = 1;
    }
    if (getenv("M20_HEADLESS")) {
        m_Headless = true;
    }

    if (!m_Headless) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        m_Window = SDL_CreateWindow(
            "Tomb Raider (m20)", SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
        m_Renderer = SDL_CreateRenderer(m_Window, -1, SDL_RENDERER_PRESENTVSYNC);
        m_Texture = SDL_CreateTexture(
            m_Renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
            M20_W, M20_H);
    }

    Shell_Main();
    S_Shell_ReportMemory("exit");
    return 0;
}

char *__wrap_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = __wrap_malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}
