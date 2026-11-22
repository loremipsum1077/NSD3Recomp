// Two C library names the source uses with the Switch libc's spelling.
#include <errno.h>
#include <stdio.h>

// The Switch libc's (and Linux's) accessor for errno. macOS calls it __error.
int * __errno_location(void)
{
    return &errno;
}

// curl's formdata.c binds to a linker symbol spelled exactly `stdin` (no platform
// prefix); on macOS stdin is a macro for another variable. Give it one, set before
// main runs.
#undef stdin
FILE * d3_stdin __asm__("stdin");
FILE * d3_stdin;

__attribute__((constructor)) static void d3_init_stdin(void)
{
    d3_stdin = __stdinp;
}
