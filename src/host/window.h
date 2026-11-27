// The host window and input devices. Everything here runs on the main thread except
// d3_input_read, which any thread may call.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Switch controller state in the Switch's own terms.
struct d3_pad_state {
    uint64_t buttons;           // bit numbers below
    int32_t leftX, leftY;       // -32767..32767, up and right positive
    int32_t rightX, rightY;
    int connected;
};

enum {
    D3_PAD_A = 0, D3_PAD_B, D3_PAD_X, D3_PAD_Y, D3_PAD_STICK_L, D3_PAD_STICK_R, D3_PAD_L, D3_PAD_R,
    D3_PAD_ZL, D3_PAD_ZR, D3_PAD_PLUS, D3_PAD_MINUS, D3_PAD_LEFT, D3_PAD_UP, D3_PAD_RIGHT, D3_PAD_DOWN
};

// Opens the window. Does nothing when D3_HEADLESS is set or no display is available.
void d3_window_create(void);

// The window's Metal layer (a CAMetalLayer), or null when there is no window.
void * d3_window_metal_layer(void);

// Handles pending window and input events. Call once per frame on the main thread.
// Returns nonzero once the user has asked to quit (closed the window).
int d3_window_pump(void);

// The current state of the one controller the port presents: a real game controller
// if one is attached, combined with the keyboard.
void d3_input_read(struct d3_pad_state * out);

// Hotkeys pressed since the last call (D3_HOTKEY_* bits), then cleared. Any thread.
// d3_hotkeys_take takes the cheat keys (F1-F3), which only the Cheats mod (D3_MOD_CHEATS)
// acts on; d3_hotkeys_take_mask takes the given ones. F5-F7 are the developer tools'
// (D3_MOD_DEV_TOOLS).
enum { D3_HOTKEY_LEVEL_MAX = 1 << 0, D3_HOTKEY_PARAGON = 1 << 1, D3_HOTKEY_GOLD = 1 << 2,
       D3_HOTKEY_CAMERA = 1 << 3, D3_HOTKEY_TARGET_NAME = 1 << 4, D3_HOTKEY_STICK_VIEW = 1 << 5,
       D3_HOTKEY_DESIGNER = 1 << 6 };
int d3_hotkeys_take(void);
int d3_hotkeys_take_mask(int mask);

// Mouse events since the last call, oldest first, with the position as a fraction of the
// window (0..1; the game's image fills the window). Any thread. Only the Mouse mod
// (D3_MOD_MOUSE) reads them; unread events beyond the queue's size are dropped.
enum { D3_MOUSE_MOVE, D3_MOUSE_DOWN, D3_MOUSE_UP, D3_MOUSE_WHEEL };
enum { D3_MOUSE_LEFT, D3_MOUSE_MIDDLE, D3_MOUSE_RIGHT };
struct d3_mouse_event {
    int type;       // D3_MOUSE_*
    int button;     // D3_MOUSE_LEFT etc., for DOWN and UP
    int wheel;      // for WHEEL: steps, up positive
    float x, y;
};
int d3_mouse_take(struct d3_mouse_event * out, int max);
// Presses a pad button (D3_PAD_*) for the given time, as if on the controller.
void d3_pad_tap(int button, int milliseconds);
// Tilts the left stick by (x, y), -32767..32767 with Y up, until set back to (0, 0).
void d3_mouse_stick(int x, int y);

#ifdef __cplusplus
}
#endif
