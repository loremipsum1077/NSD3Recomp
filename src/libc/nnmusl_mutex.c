// The Switch libc's pthread mutex, for the decomp sources that embed one: Domino,
// blz and protobuf. Reached through src/compat/d3_nnmusl_mutex_rename.h.
//
// The Switch's mutex is 32 bytes and its attribute object 8, both smaller than the
// host's. The 32 bytes hold a pointer to a host mutex. As on the Switch, an all-zero
// mutex is a valid unlocked one, so the host mutex is created on first use if init
// was never called.
#include <pthread.h>
#include <stdlib.h>

typedef union {
    pthread_mutex_t * host;
    char size[32];
} d3_nnmusl_mutex;

typedef struct {
    unsigned int type;      // the Switch libc's numbering: 1 is recursive
    unsigned int unused;
} d3_nnmusl_mutexattr;

static pthread_mutex_t g_createLock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t * Create(int recursive)
{
    pthread_mutex_t * host = malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;

    if (!host)
    {
        abort();
    }
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(host, &attr);
    pthread_mutexattr_destroy(&attr);
    return host;
}

static pthread_mutex_t * Host(d3_nnmusl_mutex * m)
{
    if (!m->host)
    {
        pthread_mutex_lock(&g_createLock);
        if (!m->host)
        {
            m->host = Create(0);
        }
        pthread_mutex_unlock(&g_createLock);
    }
    return m->host;
}

int d3_nnmusl_pthread_mutex_init(d3_nnmusl_mutex * m, const d3_nnmusl_mutexattr * attr)
{
    m->host = Create(attr && (attr->type & 3) == 1);
    return 0;
}

int d3_nnmusl_pthread_mutex_destroy(d3_nnmusl_mutex * m)
{
    if (m->host)
    {
        pthread_mutex_destroy(m->host);
        free(m->host);
        m->host = NULL;
    }
    return 0;
}

int d3_nnmusl_pthread_mutex_lock(d3_nnmusl_mutex * m)
{
    return pthread_mutex_lock(Host(m));
}

int d3_nnmusl_pthread_mutex_unlock(d3_nnmusl_mutex * m)
{
    return pthread_mutex_unlock(Host(m));
}

int d3_nnmusl_pthread_mutexattr_init(d3_nnmusl_mutexattr * attr)
{
    attr->type = 0;
    attr->unused = 0;
    return 0;
}

int d3_nnmusl_pthread_mutexattr_settype(d3_nnmusl_mutexattr * attr, int type)
{
    attr->type = (unsigned int)type;
    return 0;
}

int d3_nnmusl_pthread_mutexattr_destroy(d3_nnmusl_mutexattr * attr)
{
    (void)attr;
    return 0;
}
