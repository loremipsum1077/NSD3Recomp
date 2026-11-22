// The do-nothing graphics driver: stands where the Switch's NVN driver would.
// It lets the game start and run without drawing anything (roadmap, milestone 3).
#pragma once

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

// Called by every generated function, for tracing (see D3_NVN_TRACE in nvn_null.cpp).
void d3_nvn_called(const char * name);

#ifdef __cplusplus
}
#endif
