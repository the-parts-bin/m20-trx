// The m20 board's platform (src/m20/m20_platform.h), on the core's runtime
// (../m20-system/core/runtime: picolibc, FatFs files, the trap handler).
//
// Video: three display buffers of M20_W x M20_H RGB565 in SDRAM; the game
// draws into one (M20_Frame), M20_Present cleans it from the data cache and
// the vertical-blank interrupt shows it, every line doubled (640 x 360 is
// 2 x 2 on the 1280 x 720 picture), while drawing moves on to a buffer
// neither shown nor waiting. M20_LastFrame is the last one presented (the
// inventory's background is copied from it).
//
// Sound: the hardware plays 48 kHz from two halves of 256 frames and
// interrupts as it moves on from one; the interrupt mixes 128 frames at
// 24 kHz (S_Sound_Mix, integers only: the trap handler doesn't save the FP
// registers) and holds each for two. M20_AudioLock masks the interrupts.
//
// Input: USB keyboards on either port, as HID usages; the UART's keys too
// (a terminal only sends presses: a key counts as held until 150 ms after
// its last repeat), with the arrows from their escape sequences.
//
// Files: the game's directory on the card is TOMB1/ (DATA/, cfg/;
// src/filesystem.c adds it).
#include "m20/m20_platform.h"

#include "clock.h"      // CLOCK_PER_MS: core/firmware
#include "m9b_map.h"
#include "specific/s_hwr.h"
#include "specific/s_shell.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_CNT     (*(volatile uint32_t *)(M9B_TIMING + 0xCu))
#define UART_DATA     (*(volatile uint32_t *)M9B_UART)
#define UART_RX_VALID (1u << 8)
#define VRAM          ((volatile uint32_t *)M9B_VRAM)
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t *)M9B_CTRL)
#define IRQ_PENDING   (*(volatile uint32_t *)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t *)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile uint32_t *)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile uint32_t *)(M9B_CTRL + 0x10u))   // [0] half A, [1] B
#define VIDEO_HWIN    (*(volatile uint32_t *)(M9B_CTRL + 0x18u))
#define IRQ_VBLANK    (1u << 0)
#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)
#define LINE_BLANK    (0u << 22)
#define LINE_16BPP    (1u << 22)
#define LINE_2X       (1u << 28)
#define LINES         720u

#if M20_W > 640 || M20_H > 720 || M20_W % 4
#error the frame must fit the 1280 x 720 screen at 2x
#endif

uint16_t *M20_Frame;
uint16_t *M20_LastFrame;
uint8_t M20_Keys[256];
int M20_Argc = 1;
static char *m_ArgvMem[] = { "tomb1", NULL };
char **M20_Argv = m_ArgvMem;
int M20_StartLevel = -1;
int M20_CamTest;
uint32_t M20_ScriptKeys;
#ifdef M20_TIMEDEMO
// make TIMEDEMO=1: one game step per frame (s_clock.c), so that the title's
// demos draw the same frames each run; their time at each level load
bool M20_Scripted = true;
#else
bool M20_Scripted;
#endif

extern void trap_entry(void);       // core/runtime/trap_entry.S
extern uint32_t misaligned_traps;   // core/runtime/trap.c: emulated
extern void m20_screenshot16(uint32_t w, uint32_t h, const uint16_t *pixels,
                             uint32_t stride);   // core/runtime/shot.c

static uint32_t irq_off(void)
{
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}

static void irq_restore(uint32_t m)
{
    if (m) {
        asm volatile("csrsi mstatus, 8");
    }
}

static void cache_clean(const void *p, uint32_t len)
{
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63;
         a < (uintptr_t)p + len; a += 64u) {
        asm volatile("cbo.clean (%0)" ::"r"(a) : "memory");
    }
}

// ---------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------
static uint32_t m_ClockLast, m_ClockFrac, m_Us;

uint32_t M20_TimeUs(void)
{
    uint32_t now = CLOCK_CNT;   // (wraps every 79 s: read far more often)
    m_ClockFrac += now - m_ClockLast;
    m_ClockLast = now;
    m_Us += m_ClockFrac / CLOCK_PER_US;
    m_ClockFrac %= CLOCK_PER_US;
    return m_Us;
}

// ---------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------
#define NBUF 3
static uint16_t m_Disp[NBUF][M20_W * M20_H] __attribute__((aligned(256)));
static volatile int m_Shown = -1, m_Pending = -1;
static int m_Drawing;
static uint32_t m_RowOff[LINES];   // each line's row, in pixels
static volatile uint32_t m_VBlanks;
static uint32_t m_Frames;

static uint32_t line_16bpp(uintptr_t addr)
{
    uint32_t off = (uint32_t)addr - M9B_PSRAM_BASE;
    return LINE_16BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

// From the trap handler at each vertical blank
void vblank_flip(void)
{
    m_VBlanks++;
    int p = m_Pending;
    if (p < 0) {
        return;
    }
    const uint16_t *buf = m_Disp[p];
    for (uint32_t y = 0; y < LINES; y++) {
        VRAM_TABLE[y] = line_16bpp((uintptr_t)&buf[m_RowOff[y]]) | LINE_2X;
    }
    m_Shown = p;
    m_Pending = -1;
}

static void video_init(void)
{
    for (uint32_t y = 0; y < LINES; y++) {
        VRAM_TABLE[y] = LINE_BLANK;
        m_RowOff[y] = (y * M20_H / LINES) * M20_W;
    }
    memset(m_Disp, 0, sizeof(m_Disp));
    cache_clean(m_Disp, sizeof(m_Disp));
    VIDEO_HWIN = (640u + M20_W) << 16 | (640u - M20_W);   // centred, 2x
    VIDEO_CTRL = 1;
    m_Drawing = 0;
    M20_Frame = m_Disp[0];
    M20_LastFrame = m_Disp[0];
}

static uint32_t m_FpsStart, m_FpsFrames;
#ifdef M20_TIMEDEMO
#define TD_FRAMES 250u   // (Vilcabamba's demo is about 280)
static uint32_t m_TdFrames0, m_TdT0;
#endif
static int m_Reserved = -1;   // the display buffer holding the menus' background

// A buffer neither shown, waiting nor reserved; -1 if there's none
static int free_buffer(void)
{
    for (int n = 0; n < NBUF; n++) {
        if (n != m_Shown && n != m_Pending && n != m_Reserved
            && n != m_Drawing) {
            return n;
        }
    }
    return -1;
}

// The menus' background in one of the display buffers (the one holding
// keep, if it's one: the inventory's frame stays where it was drawn); the
// other two take the drawing until it's released
uint16_t *M20_PictureBuffer(const uint16_t *keep)
{
    if (m_Reserved >= 0 && (!keep || keep == m_Disp[m_Reserved])) {
        return m_Disp[m_Reserved];
    }
    int r = -1;
    for (int b = 0; b < NBUF; b++) {
        if (keep == m_Disp[b] && b != m_Drawing) {
            r = b;
        }
    }
    while (r < 0) {   // (one not on the screen, to be written)
        uint32_t m = irq_off();
        r = free_buffer();
        irq_restore(m);
    }
    m_Reserved = r;
    return m_Disp[r];
}

void M20_PictureRelease(void)
{
    m_Reserved = -1;
}

void M20_Present(void)
{
    int b = m_Drawing;
    cache_clean(m_Disp[b], 2u * M20_W * M20_H);
    uint32_t m = irq_off();
    m_Pending = b;
    irq_restore(m);
    int n;
    do {   // (with one reserved, until the vertical blank frees one)
        m = irq_off();
        n = 0;
        while (n < NBUF && (n == m_Shown || n == m_Pending || n == m_Reserved)) {
            n++;
        }
        irq_restore(m);
    } while (n >= NBUF);
    M20_LastFrame = m_Disp[b];
    m_Drawing = n;
    M20_Frame = m_Disp[n];
    m_Frames++;
#ifdef M20_TIMEDEMO
    if (m_Frames - m_TdFrames0 == TD_FRAMES) {
        uint32_t t = M20_TimeUs() - m_TdT0;
        printf("timedemo: %u frames in %u ms, %u.%u fps\n", (unsigned)TD_FRAMES,
               (unsigned)(t / 1000u), (unsigned)(TD_FRAMES * 1000000ull / t),
               (unsigned)(TD_FRAMES * 10000000ull / t % 10u));
    }
#endif

    // the frame rate on the UART every 5 s
    m_FpsFrames++;
    uint32_t now = M20_TimeUs();
    if (now - m_FpsStart >= 5000000u) {
        uint32_t tenths = m_FpsFrames * 10000000u / ((now - m_FpsStart) / 10u) / 10u;
        printf("%u.%u fps (%s), %u misaligned\n", (unsigned)(tenths / 10),
               (unsigned)(tenths % 10), HWR_SpanOn ? "engine" : "cpu",
               (unsigned)misaligned_traps);
        misaligned_traps = 0;
        if (m_FpsFrames && HWR_SpanOn) {
            HWR_SPAN_STATS *st = &HWR_SpanStats;
            uint32_t f = m_FpsFrames;
            printf("  engine: %u faces, %u pixels, waited %u times, %u.%u ms per frame\n",
                   (unsigned)(st->faces / f), (unsigned)(st->pixels / f),
                   (unsigned)(st->waits / f),
                   (unsigned)(st->wait_clk / f / CLOCK_PER_MS),
                   (unsigned)(st->wait_clk / f % CLOCK_PER_MS * 10 / CLOCK_PER_MS));
        }
        {   // the engine's counters (core/rtl/span_engine.v): pixels, texel
            // cache misses, busy clocks
            volatile uint32_t *sp = (volatile uint32_t *)0x490000u;
            static uint32_t p0, m0, b0;
            uint32_t pp = sp[21], mm = sp[22], bb = sp[23], f = m_FpsFrames;
            uint32_t el = (now - m_FpsStart) / 1000u;   // ms
            printf("  engine hw: %u pixels, %u misses per frame, busy %u%%\n",
                   (unsigned)((pp - p0) / f), (unsigned)((mm - m0) / f),
                   (unsigned)((bb - b0) / (CLOCK_PER_MS / 100u) / (el ? el : 1)));
            p0 = pp;
            m0 = mm;
            b0 = bb;
        }
        {
            uint32_t f = m_FpsFrames, el = now - m_FpsStart;
            printf("  per frame: %u us; list %u us, drawing %u us, the rest %u us\n",
                   (unsigned)(el / f), (unsigned)(HWR_Times.build_us / f),
                   (unsigned)(HWR_Times.draw_us / f),
                   (unsigned)((el - HWR_Times.build_us - HWR_Times.draw_us) / f));
            HWR_Times.build_us = HWR_Times.draw_us = 0;
        }
        memset(&HWR_SpanStats, 0, sizeof(HWR_SpanStats));
        m_FpsStart = now;
        m_FpsFrames = 0;
    }
}

// ---------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------
#define AUDIO_FRAMES 256u   // a half, at 48 kHz
static uint32_t m_AudioBuf[2][AUDIO_FRAMES] __attribute__((aligned(256)));
static int16_t m_Mix[AUDIO_FRAMES];   // 128 stereo frames at 24 kHz
static bool m_AudioOn;
static uint32_t m_AudioMask;
static int m_AudioDepth;

static void audio_refill(uint32_t half)
{
    uint32_t *out = m_AudioBuf[half];
    S_Sound_Mix(m_Mix, AUDIO_FRAMES / 2u);
    for (uint32_t i = 0; i < AUDIO_FRAMES / 2u; i++) {
        uint32_t w = (uint16_t)m_Mix[2u * i] | (uint32_t)(uint16_t)m_Mix[2u * i + 1u] << 16;
        out[2u * i] = w;
        out[2u * i + 1u] = w;
    }
    cache_clean(out, sizeof(m_AudioBuf[0]));
    AUDIO_PTR[half] = (uint32_t)(uintptr_t)out - M9B_PSRAM_BASE;
}

// From trap.c, with the IRQ_PENDING bits it has just cleared
void sound_irq(uint32_t pending)
{
    if (!m_AudioOn) {
        return;
    }
    if (pending & IRQ_AUDIO_A) {
        audio_refill(0);
    }
    if (pending & IRQ_AUDIO_B) {
        audio_refill(1);
    }
}

void M20_AudioInit(void)
{
    memset(m_AudioBuf, 0, sizeof(m_AudioBuf));
    cache_clean(m_AudioBuf, sizeof(m_AudioBuf));
    AUDIO_PTR[0] = (uint32_t)(uintptr_t)m_AudioBuf[0] - M9B_PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)(uintptr_t)m_AudioBuf[1] - M9B_PSRAM_BASE;
    m_AudioOn = true;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
}

void M20_AudioLock(bool lock)
{
    if (lock) {
        uint32_t m = irq_off();
        if (m_AudioDepth++ == 0) {
            m_AudioMask = m;
        }
    } else if (m_AudioDepth > 0 && --m_AudioDepth == 0) {
        irq_restore(m_AudioMask);
    }
}

// ---------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------
#define USB_REG(p, o) (*(volatile uint32_t *)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p) USB_REG(p, 0x00u)   // bits 1:0 type
#define USB_KEYS(p)   USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)  USB_REG(p, 0x08u)   // k4

static uint32_t m_UartHeld[256];   // per usage: when last seen (ms | 1), 0 none
static int m_UartEsc;              // 0, 1 after ESC, 2 after ESC [

static uint8_t uart_usage(uint32_t c)
{
    if (c >= 'a' && c <= 'z') {
        return HID_KEY(c);
    }
    if (c >= 'A' && c <= 'Z') {
        return HID_KEY(c - 'A' + 'a');
    }
    if (c >= '1' && c <= '9') {
        return HID_1 + (c - '1');
    }
    switch (c) {
    case '0': return HID_0;
    case '\r': case '\n': return HID_RETURN;
    case ' ': return HID_SPACE;
    case '\t': return HID_TAB;
    case 0x7F: case 0x08: return HID_BACKSPACE;
    case '-': return HID_MINUS;
    case '=': return HID_EQUALS;
    }
    return 0;
}

static void uart_keys(uint32_t now_ms)
{
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) {
            break;
        }
        uint32_t c = d & 0xFFu;
        uint8_t u = 0;
        if (m_UartEsc == 1) {
            m_UartEsc = c == '[' ? 2 : 0;
            if (!m_UartEsc) {
                u = HID_ESCAPE;
            }
        } else if (m_UartEsc == 2) {
            m_UartEsc = 0;
            u = c == 'A' ? HID_UP : c == 'B' ? HID_DOWN
                : c == 'C' ? HID_RIGHT : c == 'D' ? HID_LEFT : 0;
        } else if (c == 0x1B) {
            m_UartEsc = 1;
#ifdef M9B_PROFILE
        } else if (c == 0x10) {   // m9bctl.py prof: the PC samples so far
            extern void prof_report(void);
            prof_report();
#endif
        } else if (c == 0x1A) {   // Ctrl-Z: the engine's depth test off and on
            HWR_ZEngine = !HWR_ZEngine;
            printf("engine depth test %s\n", HWR_ZEngine ? "on" : "off");
        } else if (c == 0x0B) {   // Ctrl-K: the span engine off and on
            HWR_SpanOn = !HWR_SpanOn;
            printf("span engine %s\n", HWR_SpanOn ? "on" : "off");
        } else if (c == 0x1C) {   // m9bctl.py shot: the frame on screen
            if (m_Shown >= 0) {
                m20_screenshot16(M20_W, M20_H, m_Disp[m_Shown], M20_W);
            }
        } else {
            u = uart_usage(c);
        }
        if (u) {
            m_UartHeld[u] = now_ms | 1u;
        }
    }
    if (m_UartEsc == 1) {   // a lone Esc (no '[' behind it)
        m_UartEsc = 0;
        m_UartHeld[HID_ESCAPE] = now_ms | 1u;
    }
    for (int i = 0; i < 256; i++) {
        if (!m_UartHeld[i]) {
            continue;
        }
        if (now_ms - m_UartHeld[i] < 150u) {
            M20_Keys[i] = 1;
        } else {
            m_UartHeld[i] = 0;
        }
    }
}

void M20_PollInput(void)
{
    memset(M20_Keys, 0, sizeof(M20_Keys));
    for (uint32_t p = 0; p < 2u; p++) {
        if ((USB_STATUS(p) & 3u) != 1u) {   // a keyboard
            continue;
        }
        uint32_t w = USB_KEYS(p), w2 = USB_KEYS2(p);
        for (int b = 0; b < 8; b++) {
            if (w & (1u << b)) {
                M20_Keys[HID_LCTRL + b] = 1;
            }
        }
        uint8_t k[4] = { w >> 8 & 0xFFu, w >> 16 & 0xFFu, w >> 24, w2 & 0xFFu };
        for (int i = 0; i < 4; i++) {
            if (k[i] > 3) {   // (0 none, 1..3 errors)
                M20_Keys[k[i]] = 1;
            }
        }
    }
    uart_keys(M20_TimeUs() / 1000u);
}

// ---------------------------------------------------------------------
// The rest
// ---------------------------------------------------------------------
void M20_Fatal(const char *message)
{
    printf("FATAL: %s\n", message);
    S_Shell_ReportMemory("fatal");
    for (;;) {
    }
}

extern char __heap_start[], __heap_end[];

void S_Shell_ReportMemory(const char *what)
{
#ifdef M20_TIMEDEMO
    m_TdFrames0 = m_Frames;   // (the first TD_FRAMES after a level's load)
    m_TdT0 = M20_TimeUs();
#endif
    struct mallinfo mi = mallinfo();
    printf("render: peak %d primitives, %d vertices, %d dropped\n",
           (int)HWR_Stats.prims_peak, (int)HWR_Stats.verts_peak,
           (int)HWR_Stats.overflows);
    printf("heap: %s: %u KB in use, of %u KB\n", what,
           (unsigned)(mi.uordblks >> 10),
           (unsigned)((__heap_end - __heap_start) >> 10));
}

void Shell_Main(void);

int main(void)
{
    printf("\nTomb Raider (TRX) on the m20\n");
    asm volatile("csrw mtvec, %0" ::"r"(&trap_entry));
    m_ClockLast = CLOCK_CNT;
    video_init();
    IRQ_PENDING = 0xFu;
    IRQ_ENABLE |= IRQ_VBLANK;
    asm volatile("csrs mie, %0" ::"r"(1u << 11));
    asm volatile("csrs mstatus, %0" ::"r"(1u << 3));
    m_FpsStart = M20_TimeUs();
    Shell_Main();
    S_Shell_ReportMemory("exit");
    for (;;) {
    }
}
