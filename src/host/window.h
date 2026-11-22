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

#ifdef __cplusplus
}
#endif
