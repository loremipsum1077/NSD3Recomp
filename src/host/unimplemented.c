#include "unimplemented.h"

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Prints the missing function and who called it, then stops.
void d3_unimplemented(const char * name)
{
    void * frames[24];
    int count;

    fprintf(stderr, "\nNot implemented yet: %s\nCalled from:\n", name);
    count = backtrace(frames, 24);
    if (count > 2)
    {
        backtrace_symbols_fd(frames + 2, count - 2, STDERR_FILENO);
    }
    abort();
}
