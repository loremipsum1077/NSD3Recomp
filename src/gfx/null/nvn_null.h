// The do-nothing graphics driver: stands where the Switch's NVN driver would.
// It lets the game start and run without drawing anything (roadmap, milestone 3).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*d3_nvn_function)(void);

struct d3_nvn_entry {
    const char * name;
    d3_nvn_function function;
};

// One do-nothing function per entry point, generated at build time.
extern const struct d3_nvn_entry d3_nvn_defaults[];
// Entry points that need real behaviour, written by hand (nvn_null.cpp).
extern const struct d3_nvn_entry d3_nvn_overrides[];

// The Metal driver, when a window exists (../metal/nvn_metal.mm). Its entries take
// precedence over everything above.
extern const struct d3_nvn_entry d3_nvn_metal_overrides[];
int d3_nvn_metal_start(void);

// The buffer most recently created: the only way the driver learns the size of a
// shader's code, which arrives in a buffer the game makes just before handing it over.
void d3_nvn_last_buffer(void ** memory, size_t * size);

// Waits so that frames are presented at the display rate (60 a second divided by the
// interval the game asked for).
void d3_present_pace(void);

// Called by every generated function, for tracing (see D3_NVN_TRACE in nvn_null.cpp).
void d3_nvn_called(const char * name);

#ifdef __cplusplus
}
#endif
