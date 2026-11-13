// The m20 port's platform interface: what the game's specific layer
// (src/specific/) needs from the machine it runs on. Two implementations:
// src/m20/host_sdl.c (32-bit x86 Linux, SDL2: the test harness) and
// src/m20/board.c (the m20 board, M20_BOARD).
#ifndef T1M_M20_PLATFORM_H
#define T1M_M20_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#ifndef M20_W
#define M20_W 640
#endif
#ifndef M20_H
#define M20_H 360
#endif

// The frame the renderer draws, RGB565, M20_W x M20_H
extern uint16_t *M20_Frame;

// Shows the frame (M20_Frame may then be another buffer)
void M20_Present(void);

// The frame last presented (the inventory's background is copied from it)
extern uint16_t *M20_LastFrame;

// Reads the keyboard into M20_Keys: one byte per HID usage (USB HID usage
// tables, keyboard page; SDL scancodes are the same numbers), non-zero while
// held. The modifiers are usages 0xE0..0xE7 (left ctrl, shift, alt, GUI,
// then right).
void M20_PollInput(void);
extern uint8_t M20_Keys[256];

// Microseconds since start (wraps)
uint32_t M20_TimeUs(void);

// Command line
extern int M20_Argc;
extern char **M20_Argv;

// Fatal error: prints and stops
void M20_Fatal(const char *message);

// Sound output: the mixer (src/specific/s_sound.c) is asked for frames of
// signed 16-bit stereo at M20_AUDIO_RATE
#ifdef M20_BOARD
#define M20_AUDIO_RATE 24000   // (held twice: the HDMI's 48 kHz)
#else
#define M20_AUDIO_RATE 22050
#endif
void M20_AudioInit(void);
void M20_AudioLock(bool lock);   // keeps the mixer out while held
void S_Sound_Mix(int16_t *out, int frames);

// HID usages the port uses
enum {
    HID_A = 0x04, HID_Z = 0x1D,
    HID_1 = 0x1E, HID_9 = 0x26, HID_0 = 0x27,
    HID_RETURN = 0x28, HID_ESCAPE = 0x29, HID_BACKSPACE = 0x2A, HID_TAB = 0x2B,
    HID_SPACE = 0x2C, HID_MINUS = 0x2D, HID_EQUALS = 0x2E, HID_SLASH = 0x38,
    HID_F1 = 0x3A, HID_F12 = 0x45,
    HID_INSERT = 0x49, HID_HOME = 0x4A, HID_PAGEUP = 0x4B, HID_DELETE = 0x4C,
    HID_END = 0x4D, HID_PAGEDOWN = 0x4E,
    HID_RIGHT = 0x4F, HID_LEFT = 0x50, HID_DOWN = 0x51, HID_UP = 0x52,
    HID_KP_DIVIDE = 0x54, HID_KP_MULTIPLY = 0x55, HID_KP_MINUS = 0x56,
    HID_KP_PLUS = 0x57, HID_KP_ENTER = 0x58, HID_KP_1 = 0x59, HID_KP_9 = 0x61,
    HID_KP_0 = 0x62, HID_KP_PERIOD = 0x63,
    HID_LCTRL = 0xE0, HID_LSHIFT = 0xE1, HID_LALT = 0xE2, HID_LGUI = 0xE3,
    HID_RCTRL = 0xE4, HID_RSHIFT = 0xE5, HID_RALT = 0xE6, HID_RGUI = 0xE7,
};
#define HID_KEY(c) (HID_A + ((c) - 'a'))

#endif
