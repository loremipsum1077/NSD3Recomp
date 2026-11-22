// Stand-ins for functions the platform layer does not implement yet.
//
// D3_STUB(symbol, "readable name") defines the linker symbol `symbol` as a function
// that prints the name and stops the program. The stub ignores its arguments, so it
// needs no knowledge of the real signature. Stand-ins are weak: if the decomp or the
// platform layer defines the same symbol for real, the real one is used and nothing
// has to be edited here. To implement a function, delete its
// D3_STUB line and write the real one; tools/gen_stubs.py never re-adds a symbol
// that is already defined.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn)) void d3_unimplemented(const char * name);

#ifdef __cplusplus
}
#endif

#ifdef __APPLE__
#define D3_LINK_NAME(symbol) "_" #symbol
#else
#define D3_LINK_NAME(symbol) #symbol
#endif

#define D3_STUB(symbol, name)                                      \
    void d3_stub_##symbol(void) __asm__(D3_LINK_NAME(symbol));     \
    __attribute__((weak)) void d3_stub_##symbol(void)              \
    {                                                              \
        d3_unimplemented(name);                                    \
    }
