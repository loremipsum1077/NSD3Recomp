// The four mutex functions Domino (physics) calls. Domino stores the Switch libc's
// 32-byte mutex inside its own objects, which is smaller than the host's, so the
// 32 bytes hold a pointer to a host mutex instead. Domino only ever passes a null
// attribute pointer, which means a plain non-recursive mutex.
// Reached through src/compat/d3_domino_prefix.h.
#include <pthread.h>
#include <stdlib.h>

typedef union {
    pthread_mutex_t * host;
    char size[32];
} d3_nnmusl_mutex;

int d3_nnmusl_pthread_mutex_init(d3_nnmusl_mutex * m, const void * attr)
{
    (void)attr;
    m->host = malloc(sizeof(pthread_mutex_t));
    if (!m->host)
    {
        return 12; // ENOMEM
    }
    return pthread_mutex_init(m->host, NULL);
}

int d3_nnmusl_pthread_mutex_destroy(d3_nnmusl_mutex * m)
{
    int result = pthread_mutex_destroy(m->host);
    free(m->host);
    m->host = NULL;
    return result;
}

int d3_nnmusl_pthread_mutex_lock(d3_nnmusl_mutex * m)
{
    return pthread_mutex_lock(m->host);
}

int d3_nnmusl_pthread_mutex_unlock(d3_nnmusl_mutex * m)
{
    return pthread_mutex_unlock(m->host);
}
