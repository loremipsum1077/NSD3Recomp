// What the host's main loop needs from the controller code.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Tells the game when the controller appears or disappears. Call once per frame on
// the main thread, after d3_window_pump.
void d3_hid_poll(void);

#ifdef __cplusplus
}
#endif
