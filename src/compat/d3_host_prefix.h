// Included before every decomp source file (see cmake/DecompSources.cmake).
// It makes the host's basic types and libc names look like the Switch's where the
// game's source depends on the difference. Keep it small: each entry says why.
#pragma once

// 64-bit integer types. On the Switch (and Linux) int64_t is `long`; macOS makes it
// `long long`. Both are 64 bits, but they are different types to the compiler, and
// the game's source relies on uint64 and `unsigned long` being the same type
// (overloads, references, template arguments). The macOS SDK defines these two
// typedefs behind the guards below, so claiming the guards first is enough.
#if defined(__APPLE__) && defined(__LP64__)
#define _INT64_T
typedef long int64_t;
#define _UINT64_T
typedef unsigned long uint64_t;
#endif

// Headers the Switch libc pulled in indirectly and the source uses without including.
#include <stdarg.h>
#include <string.h>

#ifdef __cplusplus
#include <cstring>
// macOS defines the byte-order functions as macros; the game declares its own
// functions with these names (Blizzard/Config/Platform/NX64.h). Include the header
// that defines the macros now, so its guard stops them coming back, then drop them.
// (Not <arpa/inet.h>: the game has its own struct sockaddr.)
#ifdef __APPLE__
#include <sys/_endian.h>
#endif
#undef htons
#undef htonl
#undef ntohs
#undef ntohl
#endif
