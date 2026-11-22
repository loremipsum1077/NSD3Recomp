#include "unimplemented.h"

#include <stdio.h>
#include <stdlib.h>

void d3_unimplemented(const char * name)
{
    fprintf(stderr, "\nNot implemented yet: %s\n", name);
    abort();
}
