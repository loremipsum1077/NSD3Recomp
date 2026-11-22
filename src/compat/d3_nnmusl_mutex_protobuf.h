// protobuf's variant of d3_nnmusl_mutex_rename.h. protobuf passes the host's pointer
// type and declares nothing itself, so the routed functions are declared here.
// (Domino and blz declare them with their own types, which is why this is separate.)
#pragma once
#include "d3_nnmusl_mutex_rename.h"
extern "C" {
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
}
