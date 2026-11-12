// m20: the keyboard, as HID usages (src/m20/m20_platform.h), in place of
// DirectInput's key codes; no joystick. The built-in layout is TombATI's
// (arrows, Ctrl action, Alt jump, Shift walk, Space draw, End roll, keypad 0
// look, Delete / Page Down side steps, Esc the inventory); the user layout
// starts as letters for the same (f action, j jump, v walk, g draw, r roll,
// k look, q / e side steps). The left and right Ctrl, Shift and Alt are the
// same key.
#include "specific/s_input.h"

#include "config.h"
#include "game/inv.h"
#include "game/lara.h"
#include "global/vars.h"
#include "m20/m20_platform.h"

#include <stdbool.h>
#include <stdio.h>

extern bool M20_Scripted;
extern uint32_t M20_ScriptKeys;

#define KEY_DOWN(a) (M20_Keys[(a) & 0xFF] != 0)

static bool m_KeyConflict[INPUT_KEY_NUMBER_OF] = { false };
static S_INPUT_KEYCODE m_Layout[2][INPUT_KEY_NUMBER_OF] = {
    // clang-format off
    // built-in controls
    {
        HID_UP,          // INPUT_KEY_UP
        HID_DOWN,        // INPUT_KEY_DOWN
        HID_LEFT,        // INPUT_KEY_LEFT
        HID_RIGHT,       // INPUT_KEY_RIGHT
        HID_DELETE,      // INPUT_KEY_STEP_L
        HID_PAGEDOWN,    // INPUT_KEY_STEP_R
        HID_RSHIFT,      // INPUT_KEY_SLOW
        HID_RALT,        // INPUT_KEY_JUMP
        HID_RCTRL,       // INPUT_KEY_ACTION
        HID_SPACE,       // INPUT_KEY_DRAW
        HID_KP_0,        // INPUT_KEY_LOOK
        HID_END,         // INPUT_KEY_ROLL
        HID_ESCAPE,      // INPUT_KEY_OPTION
        HID_KEY('o'),    // INPUT_KEY_FLY_CHEAT,
        HID_KEY('i'),    // INPUT_KEY_ITEM_CHEAT,
        HID_KEY('l'),    // INPUT_KEY_LEVEL_SKIP_CHEAT,
        HID_KEY('p'),    // INPUT_KEY_PAUSE,
        HID_KEY('w'),    // INPUT_KEY_CAMERA_UP
        HID_KEY('s'),    // INPUT_KEY_CAMERA_DOWN
        HID_KEY('a'),    // INPUT_KEY_CAMERA_LEFT
        HID_KEY('d'),    // INPUT_KEY_CAMERA_RIGHT
        HID_SLASH,       // INPUT_KEY_CAMERA_RESET
    },

    // default user controls
    {
        HID_UP,          // INPUT_KEY_UP
        HID_DOWN,        // INPUT_KEY_DOWN
        HID_LEFT,        // INPUT_KEY_LEFT
        HID_RIGHT,       // INPUT_KEY_RIGHT
        HID_KEY('q'),    // INPUT_KEY_STEP_L
        HID_KEY('e'),    // INPUT_KEY_STEP_R
        HID_KEY('v'),    // INPUT_KEY_SLOW
        HID_KEY('j'),    // INPUT_KEY_JUMP
        HID_KEY('f'),    // INPUT_KEY_ACTION
        HID_KEY('g'),    // INPUT_KEY_DRAW
        HID_KEY('k'),    // INPUT_KEY_LOOK
        HID_KEY('r'),    // INPUT_KEY_ROLL
        HID_ESCAPE,      // INPUT_KEY_OPTION
        HID_KEY('o'),    // INPUT_KEY_FLY_CHEAT,
        HID_KEY('i'),    // INPUT_KEY_ITEM_CHEAT,
        HID_KEY('l'),    // INPUT_KEY_LEVEL_SKIP_CHEAT,
        HID_KEY('p'),    // INPUT_KEY_PAUSE,
        HID_KEY('w'),    // INPUT_KEY_CAMERA_UP
        HID_KEY('s'),    // INPUT_KEY_CAMERA_DOWN
        HID_KEY('a'),    // INPUT_KEY_CAMERA_LEFT
        HID_KEY('d'),    // INPUT_KEY_CAMERA_RIGHT
        HID_SLASH,       // INPUT_KEY_CAMERA_RESET
    }
    // clang-format on
};

static int32_t m_MedipackCoolDown = 0;

void S_Input_Init()
{
}

static bool m_KbdKey(INPUT_KEY key, INPUT_LAYOUT layout)
{
    S_INPUT_KEYCODE key_code = m_Layout[layout][key];
    if (KEY_DOWN(key_code)) {
        return true;
    }
    // the left and right modifiers are the same key
    if (key_code >= HID_LCTRL && key_code <= HID_RGUI) {
        return KEY_DOWN(key_code ^ 4);
    }
    return false;
}

static bool m_Key(INPUT_KEY key)
{
    return m_KbdKey(key, INPUT_LAYOUT_USER)
        || (!S_Input_IsKeyConflicted(key)
            && m_KbdKey(key, INPUT_LAYOUT_DEFAULT));
}

S_INPUT_KEYCODE S_Input_ReadKeyCode()
{
    M20_PollInput();
    for (S_INPUT_KEYCODE key = 4; key < 256; key++) {
        if (KEY_DOWN(key)) {
            return key;
        }
    }
    return -1;
}

INPUT_STATE S_Input_GetCurrentState()
{
    M20_PollInput();

    INPUT_STATE linput = { 0 };
    if (M20_Scripted) {
        // the test harness's scripted keys (src/m20/host_sdl.c)
        linput.any = M20_ScriptKeys;
        return linput;
    }

    linput.forward = m_Key(INPUT_KEY_UP);
    linput.back = m_Key(INPUT_KEY_DOWN);
    linput.left = m_Key(INPUT_KEY_LEFT);
    linput.right = m_Key(INPUT_KEY_RIGHT);
    linput.step_left = m_Key(INPUT_KEY_STEP_L);
    linput.step_right = m_Key(INPUT_KEY_STEP_R);
    linput.slow = m_Key(INPUT_KEY_SLOW);
    linput.jump = m_Key(INPUT_KEY_JUMP);
    linput.action = m_Key(INPUT_KEY_ACTION);
    linput.draw = m_Key(INPUT_KEY_DRAW);
    linput.look = m_Key(INPUT_KEY_LOOK);
    linput.roll = m_Key(INPUT_KEY_ROLL) || (linput.forward && linput.back);
    linput.option = m_Key(INPUT_KEY_OPTION) && Camera.type != CAM_CINEMATIC;
    linput.pause = m_Key(INPUT_KEY_PAUSE);
    linput.camera_up = m_Key(INPUT_KEY_CAMERA_UP);
    linput.camera_down = m_Key(INPUT_KEY_CAMERA_DOWN);
    linput.camera_left = m_Key(INPUT_KEY_CAMERA_LEFT);
    linput.camera_right = m_Key(INPUT_KEY_CAMERA_RIGHT);
    linput.camera_reset = m_Key(INPUT_KEY_CAMERA_RESET);

    if (T1MConfig.enable_cheats) {
        linput.item_cheat = m_Key(INPUT_KEY_ITEM_CHEAT);
        linput.fly_cheat = m_Key(INPUT_KEY_FLY_CHEAT);
        linput.level_skip_cheat = m_Key(INPUT_KEY_LEVEL_SKIP_CHEAT);
        linput.health_cheat = KEY_DOWN(HID_F1 + 10);   // F11
    }

    if (T1MConfig.enable_tr3_sidesteps) {
        if (linput.slow && !linput.forward && !linput.back && !linput.step_left
            && !linput.step_right) {
            if (linput.left) {
                linput.left = 0;
                linput.step_left = 1;
            } else if (linput.right) {
                linput.right = 0;
                linput.step_right = 1;
            }
        }
    }

    if (T1MConfig.enable_numeric_keys) {
        if (KEY_DOWN(HID_1) && Inv_RequestItem(O_GUN_ITEM)) {
            Lara.request_gun_type = LGT_PISTOLS;
        } else if (KEY_DOWN(HID_1 + 1) && Inv_RequestItem(O_SHOTGUN_ITEM)) {
            Lara.request_gun_type = LGT_SHOTGUN;
        } else if (KEY_DOWN(HID_1 + 2) && Inv_RequestItem(O_MAGNUM_ITEM)) {
            Lara.request_gun_type = LGT_MAGNUMS;
        } else if (KEY_DOWN(HID_1 + 3) && Inv_RequestItem(O_UZI_ITEM)) {
            Lara.request_gun_type = LGT_UZIS;
        }

        if (m_MedipackCoolDown) {
            m_MedipackCoolDown--;
        } else {
            if (KEY_DOWN(HID_1 + 7) && Inv_RequestItem(O_MEDI_OPTION)) {
                UseItem(O_MEDI_OPTION);
                m_MedipackCoolDown = FRAMES_PER_SECOND / 2;
            } else if (KEY_DOWN(HID_9) && Inv_RequestItem(O_BIGMEDI_OPTION)) {
                UseItem(O_BIGMEDI_OPTION);
                m_MedipackCoolDown = FRAMES_PER_SECOND / 2;
            }
        }
    }

    linput.select = linput.action || KEY_DOWN(HID_RETURN);
    linput.deselect = m_Key(INPUT_KEY_OPTION);

    if (linput.left && linput.right) {
        linput.left = 0;
        linput.right = 0;
    }

    if (!ModeLock && Camera.type != CAM_CINEMATIC) {
        linput.save = KEY_DOWN(HID_F1 + 4);   // F5
        linput.load = KEY_DOWN(HID_F1 + 5);   // F6
    }

    if (KEY_DOWN(HID_F1 + 1)) {   // F2
        T1MConfig.render_flags.fps_counter ^= 1;
        while (KEY_DOWN(HID_F1 + 1)) {
            M20_PollInput();
        }
    }

    return linput;
}

const char *S_Input_GetKeyCodeName(S_INPUT_KEYCODE key)
{
    static char name[2][8];
    static int which;
    char *n = name[which ^= 1];
    if (key >= HID_A && key <= HID_Z) {
        n[0] = 'A' + (key - HID_A);
        n[1] = 0;
        return n;
    }
    if (key >= HID_1 && key <= HID_9) {
        n[0] = '1' + (key - HID_1);
        n[1] = 0;
        return n;
    }
    if (key >= HID_F1 && key <= HID_F12) {
        snprintf(n, 8, "F%d", key - HID_F1 + 1);
        return n;
    }
    if (key >= HID_KP_1 && key <= HID_KP_9) {
        snprintf(n, 8, "PAD%d", key - HID_KP_1 + 1);
        return n;
    }
    // clang-format off
    switch (key) {
        case HID_0:           return "0";
        case HID_RETURN:      return "RETURN";
        case HID_ESCAPE:      return "ESC";
        case HID_BACKSPACE:   return "BKSP";
        case HID_TAB:         return "TAB";
        case HID_SPACE:       return "SPACE";
        case HID_MINUS:       return "-";
        case HID_EQUALS:      return "=";
        case 0x2F:            return "[";
        case 0x30:            return "]";
        case 0x31:            return "\\";
        case 0x33:            return ";";
        case 0x34:            return "'";
        case 0x35:            return "`";
        case 0x36:            return ",";
        case 0x37:            return ".";
        case HID_SLASH:       return "/";
        case HID_INSERT:      return "INS";
        case HID_HOME:        return "HOME";
        case HID_PAGEUP:      return "PGUP";
        case HID_DELETE:      return "DEL";
        case HID_END:         return "END";
        case HID_PAGEDOWN:    return "PGDN";
        case HID_UP:          return "UP";
        case HID_DOWN:        return "DOWN";
        case HID_LEFT:        return "LEFT";
        case HID_RIGHT:       return "RIGHT";
        case HID_KP_DIVIDE:   return "PAD/";
        case HID_KP_MULTIPLY: return "PAD*";
        case HID_KP_MINUS:    return "PAD-";
        case HID_KP_PLUS:     return "PAD+";
        case HID_KP_ENTER:    return "ENTER";
        case HID_KP_0:        return "PAD0";
        case HID_KP_PERIOD:   return "PAD.";
        case HID_LCTRL:       return "CTRL";
        case HID_LSHIFT:      return "SHIFT";
        case HID_LALT:        return "ALT";
        case HID_RCTRL:       return "CTRL";
        case HID_RSHIFT:      return "SHIFT";
        case HID_RALT:        return "ALT";
    }
    // clang-format on
    return "????";
}

bool S_Input_IsKeyConflicted(INPUT_KEY key)
{
    return m_KeyConflict[key];
}

void S_Input_SetKeyAsConflicted(INPUT_KEY key, bool is_conflicted)
{
    m_KeyConflict[key] = is_conflicted;
}

S_INPUT_KEYCODE S_Input_GetAssignedKeyCode(int16_t layout_num, INPUT_KEY key)
{
    return m_Layout[layout_num][key];
}

void S_Input_AssignKeyCode(
    int16_t layout_num, INPUT_KEY key, S_INPUT_KEYCODE key_code)
{
    m_Layout[layout_num][key] = key_code;
}
