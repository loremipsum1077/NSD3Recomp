// Memory for the sound code's own copy of the Ogg Vorbis decoder.
//
// The decomp's Vorbis library takes its memory from four functions the game supplies
// (in X/Client/Cutscene.cpp), which draw on one of the game's heaps. That heap has a
// fixed size and is not safe to use from the sound loader's thread, so the sound code
// is built with a second, private copy of the library whose memory comes from the
// host instead (see CMakeLists.txt, d3_sound).
#include <malloc/malloc.h>

void * _ogg_malloc(int size) { return malloc_zone_malloc(malloc_default_zone(), (size_t)size); }
void * _ogg_calloc(int count, int size) { return malloc_zone_calloc(malloc_default_zone(), (size_t)count, (size_t)size); }
void * _ogg_realloc(void * p, int size) { return malloc_zone_realloc(malloc_default_zone(), p, (size_t)size); }
void _ogg_free(void * p) { malloc_zone_free(malloc_default_zone(), p); }
