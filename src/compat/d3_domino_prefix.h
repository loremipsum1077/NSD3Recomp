// Included before Domino's sources only. Domino declares four pthread functions
// against the Switch libc's 32-byte mutex, which conflicts with the host's
// <pthread.h>. Route those four calls to the platform layer instead, which
// implements them for the 32-byte type (src/nn, milestone 2).
#pragma once
#include <pthread.h>
// The C++ library calls the real functions from its own headers; read those first
// so the renaming below only reaches Domino's code.
#include <memory>
#include <mutex>
#include <thread>
#define pthread_mutex_init d3_nnmusl_pthread_mutex_init
#define pthread_mutex_destroy d3_nnmusl_pthread_mutex_destroy
#define pthread_mutex_lock d3_nnmusl_pthread_mutex_lock
#define pthread_mutex_unlock d3_nnmusl_pthread_mutex_unlock
