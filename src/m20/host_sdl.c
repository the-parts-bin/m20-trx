// The test harness's platform (src/m20/m20_platform.h): 32-bit x86 Linux
// with SDL2, the board's frame size and heap.
//
// The span engine: the renderer writes the board's registers
// (s_hwr_span.h), which go to core's emulator of the engine
// (core/firmware/span_emu.c, M20_SPANEMU: make host); its counts and
// warnings (spans of 0 pixels, z buffers it can't address, stale z cache
// lines) are printed at the exit. The program is linked at 0x800000
// (-Ttext-segment), so its image, the frame, the depth buffer, the lightmap
// ring and the heap arena (the level's texture pages) sit in [0x800000,
// 0x1000000), the window that stands for the 8 MB of SDRAM: the board's
// SDRAM_OFF(p) = p - 0x800000 holds (M20_SdramOff checks it), and
// span_emu_init((void *)0x800000). make host SPANEMU=0: the engine's C
// model instead (span_core.h, above the register level; M20_TRACE).
//
// Files: the game's files from M20_DATA (the SD card's root, holding
// TOMB1/, or TOMB1/ itself: DATA/ and cfg/; default the current directory);
// everything the game writes (saves, cfg/Tomb1Main_runtime.json5,
// Tomb1Main.log) and the harness's pictures and traces go to M20_OUT
// (default the current directory; made if it isn't there), which is also
// read first (a file the game wrote there wins over the card's).
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
// gameflow's numbers: 0 Lara's Home, 1 Caves, ... 15 the Great Pyramid);
// M20_ZBUF=0 sorted (the default), 1 the depth buffer, 2 both, compared:
// each frame's count of differing pixels on stderr ("zdiff"), and with each
// saved frame the sorted one as fNNNNN-s.ppm (F8 in the window cycles them);
// M20_SPAN=0/1 the span engine off/on for the opaque textured faces (on by
// default with the emulator, off with SPANEMU=0; F7 in the window toggles
// it); M20_EMUSTATS=n span_emu_stats every n frames too.
#include "m20/m20_platform.h"
#include "specific/s_hwr.h"

#include <SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef M20_SPANEMU
#include "span_emu.h"
#endif

// (aligned as the board's display buffers: the depth buffer a multiple of
// 2 KB from it, as the engine addresses it)
static uint16_t m_FrameBuf[M20_W * M20_H] __attribute__((aligned(2048)));
uint16_t *M20_Frame;
uint16_t *M20_LastFrame;
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
static int m_EmuStats;       // M20_EMUSTATS: every n frames
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
// The SDRAM window, the emulator
// ---------------------------------------------------------------------
#define WIN_LO 0x800000u
#define WIN_HI 0x1000000u
extern char __executable_start[], _end[];

uint32_t M20_SdramOff(const void *p, const char *what)
{
    uintptr_t a = (uintptr_t)p;
    if (a < WIN_LO || a >= WIN_HI) {
        fprintf(
            stderr,
            "SDRAM_OFF(%s): %p is outside the SDRAM window [0x800000, "
            "0x1000000) (frame %d)\n",
            what, p, m_FrameNo);
        abort();
    }
    return (uint32_t)a - WIN_LO;
}

static void emu_stats(const char *when)
{
#ifdef M20_SPANEMU
    const span_emu_stats_t *s = &span_emu_stats;
    fprintf(
        stderr,
        "span_emu %s (frame %d): spans %u pixels %u (in %u) edges %u "
        "lightmaps %u zero_spans %u z_layout %u z_stale %u\n",
        when, m_FrameNo, s->spans, s->pixels, s->pixels_in, s->edges,
        s->lightmaps, s->zero_spans, s->z_layout, s->z_stale);
#endif
}

static void host_exit(int code, const char *what)
{
    fflush(stdout);
    S_Shell_ReportMemory(what);
    emu_stats("at the exit");
    exit(code);
}

// ---------------------------------------------------------------------
// Files: read from M20_OUT (the current directory), then M20_DATA; written
// to M20_OUT, with the directories they need (the game's fopen calls)
// ---------------------------------------------------------------------
static char m_DataDir[PATH_MAX];   // "" when it's M20_OUT
static char m_OutDir[PATH_MAX];
FILE *__real_fopen(const char *path, const char *mode);

static void make_dirs(const char *path)   // the directories of path's file
{
    char d[PATH_MAX];
    snprintf(d, sizeof(d), "%s", path);
    for (char *p = d + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(d, 0777);
            *p = '/';
        }
    }
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (path[0] == '/') {
        return __real_fopen(path, mode);
    }
    if (strpbrk(mode, "wa+")) {
        make_dirs(path);
        return __real_fopen(path, mode);
    }
    FILE *f = __real_fopen(path, mode);
    if (!f && m_DataDir[0]) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", m_DataDir, path);
        f = __real_fopen(full, mode);
    }
    return f;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void make_dir_path(const char *path)   // mkdir -p
{
    char d[PATH_MAX];
    snprintf(d, sizeof(d), "%s/", path);
    make_dirs(d);
}

// M20_DATA (the card's root or TOMB1/) and M20_OUT, absolute, and into
// M20_OUT
static void setup_dirs(void)
{
    char cwd[PATH_MAX], data[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, ".");
    }
    const char *d = getenv("M20_DATA");
    if (!d || !*d) {
        d = cwd;
    }
    char tomb1[PATH_MAX];
    snprintf(tomb1, sizeof(tomb1), "%s/TOMB1", d);
    if (is_dir(tomb1)) {
        d = tomb1;
    }
    if (!realpath(d, data)) {
        fprintf(stderr, "host: no data directory %s: %s\n", d, strerror(errno));
        exit(1);
    }
    const char *o = getenv("M20_OUT");
    if (!o || !*o) {
        o = cwd;
    }
    make_dir_path(o);
    if (!realpath(o, m_OutDir) || chdir(m_OutDir) != 0) {
        fprintf(stderr, "host: no output directory %s: %s\n", o, strerror(errno));
        exit(1);
    }
    if (strcmp(data, m_OutDir)) {
        snprintf(m_DataDir, sizeof(m_DataDir), "%s", data);
    }
    char cfg[PATH_MAX];
    snprintf(cfg, sizeof(cfg), "%s/cfg/Tomb1Main_gameflow.json5", data);
    if (access(cfg, R_OK) != 0) {
        fprintf(stderr, "host: warning: no %s (M20_DATA: the card's root or TOMB1/)\n", cfg);
    }
    fprintf(stderr, "host: data %s, output %s\n", data, m_OutDir);
}

// ---------------------------------------------------------------------
// Video, input, time
// ---------------------------------------------------------------------
static void save_one(const uint16_t *frame, const char *suffix)
{
    char name[32];
    snprintf(name, sizeof(name), "f%05d%s.ppm", m_FrameNo, suffix);
    FILE *f = fopen(name, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", M20_W, M20_H);
    for (int i = 0; i < M20_W * M20_H; i++) {
        uint16_t c = frame[i];
        uint8_t rgb[3] = { (uint8_t)((c >> 11) << 3),
                           (uint8_t)(((c >> 5) & 63) << 2),
                           (uint8_t)((c & 31) << 3) };
        fwrite(rgb, 3, 1, f);
    }
    fclose(f);
    fprintf(stderr, "saved %s\n", name);
}

static void save_ppm(void)
{
    save_one(M20_Frame, "");
    if (HWR_ZMode == 2) {
        save_one(HWR_SortedFrame(), "-s");
    }
    if (HWR_ZMode && getenv("M20_ZDUMP")) {   // the depth buffer, 16-bit PGM
        char name[32];
        snprintf(name, sizeof(name), "f%05d-z.pgm", m_FrameNo);
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P5\n%d %d\n65535\n", M20_W, M20_H);
            const uint16_t *z = HWR_DepthBuffer();
            for (int i = 0; i < M20_W * M20_H; i++) {
                uint8_t b[2] = { z[i] >> 8, z[i] & 0xFF };
                fwrite(b, 2, 1, f);
            }
            fclose(f);
        }
    }
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

// (outside the SDRAM window, which it wouldn't fit in with the rest: the
// engine never sees it)
void *__real_malloc(size_t size);
uint16_t *M20_PictureBuffer(const uint16_t *keep)
{
    static uint16_t *picture;
    if (!picture) {
        picture = __real_malloc(M20_W * M20_H * 2);
    }
    return picture;
}

void M20_PictureRelease(void)
{
}

// M20_TRACE=a,b,c: the span engine's command trace of those frames
// (s_hwr_span.h), into trace-fNNNNN.bin (SPANEMU=0 only)
#ifndef M20_SPANEMU
extern FILE *HWR_SpanTrace;
void HWR_SpanTraceStart(void);
#endif
static char m_Trace[256];

void M20_Present(void)
{
#ifndef M20_SPANEMU
    if (HWR_SpanTrace) {
        fclose(HWR_SpanTrace);
        HWR_SpanTrace = NULL;
    }
#endif
    if (HWR_ZMode == 2 && HWR_Stats.zdiff) {
        fprintf(stderr, "zdiff %d %d\n", m_FrameNo, HWR_Stats.zdiff);
        HWR_Stats.zdiff = 0;
    }
    if (m_Steps >= 0) {
        if (listed(m_Shots, m_FrameNo)) {
            save_ppm();
        }
        if (m_FrameNo >= m_Steps) {
            host_exit(0, "exit");
        }
    }
    if (m_EmuStats && m_FrameNo % m_EmuStats == 0) {
        emu_stats("so far");
    }
    if (!m_Headless) {
        SDL_UpdateTexture(m_Texture, NULL, M20_Frame, M20_W * 2);
        SDL_RenderClear(m_Renderer);
        SDL_RenderCopy(m_Renderer, m_Texture, NULL, NULL);
        SDL_RenderPresent(m_Renderer);
    }
    m_FrameNo++;
#ifndef M20_SPANEMU
    if (m_Trace[0] && listed(m_Trace, m_FrameNo)) {
        char name[64];
        snprintf(name, sizeof(name), "trace-f%05d.bin", m_FrameNo);
        HWR_SpanTrace = fopen(name, "wb");
        HWR_SpanTraceStart();
    }
#endif
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
            host_exit(0, "exit");
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12
            && !e.key.repeat) {
            save_ppm();
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F6
            && !e.key.repeat) {
            HWR_ZEngine = !HWR_ZEngine;
            fprintf(stderr, "engine depth test: %s\n", HWR_ZEngine ? "on" : "off");
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F7
            && !e.key.repeat) {
            HWR_SpanOn = !HWR_SpanOn;
            fprintf(stderr, "span engine: %s\n", HWR_SpanOn ? "on" : "off");
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F8
            && !e.key.repeat) {
            HWR_ZMode = (HWR_ZMode + 1) % 3;
            static const char *names[] = { "sorted", "depth buffer", "compare" };
            fprintf(stderr, "renderer: %s\n", names[HWR_ZMode]);
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
    host_exit(1, "fatal");
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
    if ((uintptr_t)__executable_start < WIN_LO || (uintptr_t)_end > WIN_HI) {
        fprintf(
            stderr,
            "host: the image [%p, %p) isn't inside the SDRAM window "
            "[0x800000, 0x1000000)\n",
            (void *)__executable_start, (void *)_end);
        return 1;
    }
#ifdef M20_SPANEMU
    span_emu_init((void *)(uintptr_t)WIN_LO);
#endif
    M20_Frame = m_FrameBuf;
    M20_LastFrame = m_FrameBuf;
    setup_dirs();

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
    if (getenv("M20_TRACE")) {
#ifdef M20_SPANEMU
        fprintf(stderr, "host: M20_TRACE needs make host SPANEMU=0\n");
#else
        snprintf(m_Trace, sizeof(m_Trace), "%s", getenv("M20_TRACE"));
#endif
    }
    if (getenv("M20_EMUSTATS")) {
        m_EmuStats = atoi(getenv("M20_EMUSTATS"));
    }
    if (getenv("M20_SPAN")) {
        HWR_SpanOn = atoi(getenv("M20_SPAN")) != 0;
    }
    if (getenv("M20_ZENGINE")) {
        HWR_ZEngine = atoi(getenv("M20_ZENGINE")) != 0;
    }
    if (getenv("M20_ZBUF")) {
        HWR_ZMode = atoi(getenv("M20_ZBUF"));
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

    fprintf(
        stderr, "host: image %p-%p, heap arena %p-%p (%u KB), span engine %s\n",
        (void *)__executable_start, (void *)_end, (void *)m_Arena,
        (void *)(m_Arena + M20_HEAP), (unsigned)(M20_HEAP >> 10),
#ifdef M20_SPANEMU
        "emulated (span_emu)"
#else
        "C model (span_core)"
#endif
    );
    Shell_Main();
    host_exit(0, "exit");
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
