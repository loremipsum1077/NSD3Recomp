// Included before the few decomp sources that use the Switch libc's pthread mutex:
// Domino, blz and protobuf (see cmake/DecompSources.cmake).
//
// That mutex is 32 bytes and its attribute object 8; the host's are larger (64 and
// 16 on macOS). Those sources reserve the Switch's sizes inside their own objects,
// so letting them call the host's functions overruns the storage. The calls are
// routed to the platform layer instead, which implements them for the Switch's
// sizes (src/libc/nnmusl_mutex.c).
#pragma once
#include <pthread.h>
// The C++ library calls the real functions from its own headers; read those first
// so the renaming below only reaches the decomp's code.
#ifdef __cplusplus
#include <memory>
#include <mutex>
#include <thread>
#endif
#define pthread_mutex_init d3_nnmusl_pthread_mutex_init
#define pthread_mutex_destroy d3_nnmusl_pthread_mutex_destroy
#define pthread_mutex_lock d3_nnmusl_pthread_mutex_lock
#define pthread_mutex_unlock d3_nnmusl_pthread_mutex_unlock
#define pthread_mutexattr_init d3_nnmusl_pthread_mutexattr_init
#define pthread_mutexattr_settype d3_nnmusl_pthread_mutexattr_settype
#define pthread_mutexattr_destroy d3_nnmusl_pthread_mutexattr_destroy
