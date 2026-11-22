// nn::os locks, events and semaphores.
//
// The game's objects are small (a light event is 12 bytes), too small to hold a host
// mutex and condition variable. So the state lives in the game's object and the
// waiting is done on a shared table of host mutex/condition pairs, picked by the
// object's address. Correct and portable; contended locks are slower than they
// could be, which is a known cost to revisit if profiling shows it.
#include "os_types.h"

#include <errno.h>
#include <time.h>

namespace {

struct Lot {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
};
Lot g_lots[251];

Lot & LotFor(const void * object)
{
    return g_lots[(reinterpret_cast<uintptr_t>(object) >> 3) % 251];
}

struct Guard {
    Lot & lot;
    explicit Guard(const void * object) : lot(LotFor(object)) { pthread_mutex_lock(&lot.mutex); }
    ~Guard() { pthread_mutex_unlock(&lot.mutex); }
    void Wait() { pthread_cond_wait(&lot.cond, &lot.mutex); }
    // False when the time ran out.
    bool WaitUntil(const timespec & deadline) { return pthread_cond_timedwait(&lot.cond, &lot.mutex, &deadline) != ETIMEDOUT; }
    void WakeAll() { pthread_cond_broadcast(&lot.cond); }
};

timespec DeadlineAfter(nn::TimeSpan span)
{
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ns = now.tv_nsec + (span.ns > 0 ? span.ns : 0);
    now.tv_sec += ns / 1000000000;
    now.tv_nsec = ns % 1000000000;
    return now;
}

template <class Event> void Init(Event * e, bool signaled, nn::os::EventClearMode mode)
{
    e->signaled = signaled;
    e->autoClear = mode == nn::os::EventClearMode_AutoClear;
}

template <class Event> void Signal(Event * e)
{
    Guard g(e);
    e->signaled = true;
    g.WakeAll();
}

template <class Event> void Clear(Event * e)
{
    Guard g(e);
    e->signaled = false;
}

template <class Event> bool TryWait(Event * e)
{
    Guard g(e);
    if (!e->signaled)
    {
        return false;
    }
    if (e->autoClear)
    {
        e->signaled = false;
    }
    return true;
}

template <class Event> void Wait(Event * e)
{
    Guard g(e);
    while (!e->signaled)
    {
        g.Wait();
    }
    if (e->autoClear)
    {
        e->signaled = false;
    }
}

template <class Event> bool TimedWait(Event * e, nn::TimeSpan timeout)
{
    timespec deadline = DeadlineAfter(timeout);
    Guard g(e);
    while (!e->signaled)
    {
        if (!g.WaitUntil(deadline))
        {
            return false;
        }
    }
    if (e->autoClear)
    {
        e->signaled = false;
    }
    return true;
}

}  // namespace

namespace nn { namespace os {

// ---- mutex ----

void InitializeMutex(MutexType * m, bool recursive, int)
{
    m->owner = nullptr;
    m->nestCount = 0;
    m->recursive = recursive;
}

void FinalizeMutex(MutexType *)
{
}

void LockMutex(MutexType * m)
{
    ThreadType * self = GetCurrentThread();
    Guard g(m);
    if (m->owner == self && m->recursive)
    {
        m->nestCount++;
        return;
    }
    while (m->owner)
    {
        g.Wait();
    }
    m->owner = self;
    m->nestCount = 1;
}

bool TryLockMutex(MutexType * m)
{
    ThreadType * self = GetCurrentThread();
    Guard g(m);
    if (m->owner == self && m->recursive)
    {
        m->nestCount++;
        return true;
    }
    if (m->owner)
    {
        return false;
    }
    m->owner = self;
    m->nestCount = 1;
    return true;
}

void UnlockMutex(MutexType * m)
{
    Guard g(m);
    if (--m->nestCount == 0)
    {
        m->owner = nullptr;
        g.WakeAll();
    }
}

// ---- events ----

void InitializeEvent(EventType * e, bool signaled, EventClearMode mode) { Init(e, signaled, mode); }
void FinalizeEvent(EventType *) {}
void SignalEvent(EventType * e) { Signal(e); }
void ClearEvent(EventType * e) { Clear(e); }
bool TryWaitEvent(EventType * e) { return TryWait(e); }
void WaitEvent(EventType * e) { Wait(e); }

void InitializeLightEvent(LightEventType * e, bool signaled, EventClearMode mode) { Init(e, signaled, mode); }
void FinalizeLightEvent(LightEventType *) {}
void SignalLightEvent(LightEventType * e) { Signal(e); }
void WaitLightEvent(LightEventType * e) { Wait(e); }
bool TimedWaitLightEvent(LightEventType * e, TimeSpan timeout) { return TimedWait(e, timeout); }

// System events are created by other SDK areas; these three only use the event part.
void ClearSystemEvent(SystemEventType * e) { Clear(&e->event); }
bool TryWaitSystemEvent(SystemEventType * e) { return TryWait(&e->event); }
void DestroySystemEvent(SystemEventType *) {}

// ---- semaphore ----

void InitializeSemaphore(SemaphoreType * s, int initialCount, int maxCount)
{
    s->count = initialCount;
    s->maxCount = maxCount;
}

void FinalizeSemaphore(SemaphoreType *)
{
}

void AcquireSemaphore(SemaphoreType * s)
{
    Guard g(s);
    while (s->count == 0)
    {
        g.Wait();
    }
    s->count--;
}

bool TimedAcquireSemaphore(SemaphoreType * s, TimeSpan timeout)
{
    timespec deadline = DeadlineAfter(timeout);
    Guard g(s);
    while (s->count == 0)
    {
        if (!g.WaitUntil(deadline))
        {
            return false;
        }
    }
    s->count--;
    return true;
}

void ReleaseSemaphore(SemaphoreType * s)
{
    Guard g(s);
    s->count++;
    g.WakeAll();
}

// ---- reader/writer lock ----
// The thread holding the write lock may take it again, and may take read locks.

void InitializeReaderWriterLock(ReaderWriterLockType * l)
{
    l->writer = nullptr;
    l->writeNest = 0;
    l->readers = 0;
}

void FinalizeReaderWriterLock(ReaderWriterLockType *)
{
}

void AcquireReadLock(ReaderWriterLockType * l)
{
    ThreadType * self = GetCurrentThread();
    Guard g(l);
    while (l->writer && l->writer != self)
    {
        g.Wait();
    }
    l->readers++;
}

void ReleaseReadLock(ReaderWriterLockType * l)
{
    Guard g(l);
    l->readers--;
    g.WakeAll();
}

void AcquireWriteLock(ReaderWriterLockType * l)
{
    ThreadType * self = GetCurrentThread();
    Guard g(l);
    if (l->writer == self)
    {
        l->writeNest++;
        return;
    }
    while (l->writer || l->readers)
    {
        g.Wait();
    }
    l->writer = self;
    l->writeNest = 1;
}

void ReleaseWriteLock(ReaderWriterLockType * l)
{
    Guard g(l);
    if (--l->writeNest == 0)
    {
        l->writer = nullptr;
        g.WakeAll();
    }
}

}}  // namespace nn::os

// The SDK's C spellings of the same functions (used by curl).
extern "C" {
void nnosInitializeMutex(nn::os::MutexType * m, bool recursive, int level) { nn::os::InitializeMutex(m, recursive, level); }
void nnosFinalizeMutex(nn::os::MutexType * m) { nn::os::FinalizeMutex(m); }
void nnosLockMutex(nn::os::MutexType * m) { nn::os::LockMutex(m); }
void nnosUnlockMutex(nn::os::MutexType * m) { nn::os::UnlockMutex(m); }
void nnosInitializeEvent(nn::os::EventType * e, bool signaled, int mode) { nn::os::InitializeEvent(e, signaled, static_cast<nn::os::EventClearMode>(mode)); }
void nnosFinalizeEvent(nn::os::EventType * e) { nn::os::FinalizeEvent(e); }
void nnosSignalEvent(nn::os::EventType * e) { nn::os::SignalEvent(e); }
void nnosWaitEvent(nn::os::EventType * e) { nn::os::WaitEvent(e); }
}
