// nn::diag and friends: logging, aborts, and a few one-line services.
#include "nn/nn_types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace nn { namespace diag {

struct LogMetaData;
struct AbortInfo;
struct AbortObserverHolder;

bool IsDebuggerAttached()
{
    return false;
}

// Abort observers let the game add to the Switch's crash screen. The host's crash
// reporter (src/host/crash.c) covers that, so observers are accepted and unused.
void InitializeAbortObserverHolder(AbortObserverHolder *, void (*)(const AbortInfo &)) {}
void RegisterAbortObserver(AbortObserverHolder *) {}
void EnableDefaultAbortObserver(bool) {}

namespace detail {

void LogImpl(const LogMetaData &, const char * format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

__attribute__((noreturn)) void AbortImpl(const char * condition, const char * function, const char * file, int line)
{
    fprintf(stderr, "\nThe game aborted: %s\n  in %s (%s:%d)\n", condition ? condition : "", function ? function : "?",
            file ? file : "?", line);
    abort();
}

__attribute__((noreturn)) void AbortImpl(const char * condition, const char * function, const char * file, int line,
                                         const Result * result, const char * format, ...)
{
    fprintf(stderr, "\nThe game aborted: %s\n  in %s (%s:%d)\n  ", condition ? condition : "", function ? function : "?",
            file ? file : "?", line);
    if (result)
    {
        fprintf(stderr, "result 0x%08x: ", result->value);
    }
    if (format)
    {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
    fputc('\n', stderr);
    abort();
}

}  // namespace detail
}}  // namespace nn::diag

namespace nn { namespace detail {

__attribute__((noreturn)) void UnexpectedDefaultImpl(const char * function, const char * file, int line)
{
    fprintf(stderr, "\nThe game reached a switch case it treats as impossible, in %s (%s:%d)\n", function ? function : "?",
            file ? file : "?", line);
    abort();
}

}}  // namespace nn::detail

namespace nn { namespace util {

// Keeps a symbol from being discarded by the Switch's linker; nothing to do here.
void ReferSymbol(const void *)
{
}

}}  // namespace nn::util

namespace nn { namespace crypto {

void GenerateCryptographicallyRandomBytes(void * buffer, unsigned long size)
{
    arc4random_buf(buffer, size);
}

}}  // namespace nn::crypto

// C spellings (used by curl).
extern "C" {

__attribute__((noreturn)) void nndetailDiagAbortFormatImpl(const char * condition, const char * function, const char * file, int line,
                                                           const char * format, ...)
{
    fprintf(stderr, "\nThe game aborted: %s\n  in %s (%s:%d)\n  ", condition ? condition : "", function ? function : "?",
            file ? file : "?", line);
    if (format)
    {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
    fputc('\n', stderr);
    abort();
}

bool nnResultIsSuccess(uint32_t result) { return result == 0; }
bool nnResultIsFailure(uint32_t result) { return result != 0; }
int nnResultGetModule(uint32_t result) { return result & 0x1ff; }
int nnResultGetDescription(uint32_t result) { return (result >> 9) & 0x1fff; }

}
