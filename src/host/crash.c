// Prints what happened and the call stack when the program crashes. Installed
// before the game's static initialisers run, so crashes there are reported too.
#include <execinfo.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static char g_altStack[64 * 1024];

static void OnCrash(int sig, siginfo_t * info, void * context)
{
    void * frames[48];
    char line[128];
    int count;

    snprintf(line, sizeof line, "\nCrash: %s at address %p\nCall stack:\n",
             sig == SIGSEGV ? "invalid memory access" : sig == SIGBUS ? "bus error" : sig == SIGILL ? "illegal instruction"
                                                                                  : "arithmetic error",
             info->si_addr);
    write(STDERR_FILENO, line, strlen(line));
#if defined(__APPLE__) && defined(__aarch64__)
    {
        // The faulting function itself is often missing from the stack below.
        ucontext_t * uc = (ucontext_t *)context;
        void * pc[2] = {(void *)uc->uc_mcontext->__ss.__pc, (void *)uc->uc_mcontext->__ss.__lr};
        write(STDERR_FILENO, "Faulting instruction, then its caller:\n", 39);
        backtrace_symbols_fd(pc, 2, STDERR_FILENO);
        for (int i = 0; i < 29; i++)
        {
            snprintf(line, sizeof line, "x%-2d %016llx%s", i, (unsigned long long)uc->uc_mcontext->__ss.__x[i], i % 4 == 3 ? "\n" : "  ");
            write(STDERR_FILENO, line, strlen(line));
        }
        write(STDERR_FILENO, "\n", 1);
    }
#else
    (void)context;
#endif
    count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#ifdef __APPLE__
    // For source lines: atos -o build/macos/d3 -s <slide> <address>...
    snprintf(line, sizeof line, "Image slide: 0x%lx\n", (unsigned long)_dyld_get_image_vmaddr_slide(0));
    write(STDERR_FILENO, line, strlen(line));
#endif
    _exit(128 + sig);
}

__attribute__((constructor(101))) static void InstallCrashHandler(void)
{
    stack_t stack;
    struct sigaction action;

    stack.ss_sp = g_altStack;
    stack.ss_size = sizeof g_altStack;
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);

    memset(&action, 0, sizeof action);
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}
