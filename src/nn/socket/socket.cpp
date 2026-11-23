// nn::socket. There is no network: the port does not talk to the game's online
// services, which need a console's credentials. Start-up, shutdown, the byte-order
// helpers and address formatting are real; every call that would open or use a
// connection behaves like a console with no network attached: sockets can be made
// and set up, what is sent goes nowhere, nothing ever arrives, and connecting fails
// with "network is unreachable". (Failing socket creation itself is not an option:
// the game's network library then does not start, and later code assumes it did.)
#include "nn/nn_types.h"

#include <time.h>

namespace nn { namespace socket {

class Config;

Result Initialize(const Config &)
{
    return ResultSuccess();
}

Result Finalize()
{
    return ResultSuccess();
}

unsigned int InetHtonl(unsigned int value) { return __builtin_bswap32(value); }
unsigned short InetHtons(unsigned short value) { return __builtin_bswap16(value); }
unsigned int InetNtohl(unsigned int value) { return __builtin_bswap32(value); }
unsigned short InetNtohs(unsigned short value) { return __builtin_bswap16(value); }

}}  // namespace nn::socket

// The Switch's own socket types; only pointers to them pass through here.
struct sockaddr;
struct addrinfo;
struct hostent;
struct pollfd;
struct in_addr { unsigned int s_addr; };

namespace nn { namespace socket {

const int kWouldBlock = 35;         // EAGAIN: nothing to read yet (the same numbers as on the Switch)
const int kUnreachable = 51;        // ENETUNREACH
static thread_local int t_error;
static int g_nextSocket = 3;

static int Fail(int error)
{
    t_error = error;
    return -1;
}

int GetLastErrno() { return t_error; }
int Socket(int, int, int) { return __atomic_fetch_add(&g_nextSocket, 1, __ATOMIC_RELAXED) & 0x3ff; }
int Close(int) { return 0; }
int Bind(int, const sockaddr *, unsigned int) { return 0; }
int Connect(int, const sockaddr *, unsigned int) { return Fail(kUnreachable); }
int Fcntl(int, int, ...) { return 0; }
int SetSockOpt(int, int, int, const void *, unsigned int) { return 0; }
int Poll(pollfd *, unsigned long, int timeoutMs)
{
    // Nothing ever becomes ready: wait as asked (briefly, so that no caller spins) and say so.
    timespec wait = {0, (timeoutMs < 0 || timeoutMs > 50 ? 50 : timeoutMs) * 1000000L};
    nanosleep(&wait, nullptr);
    return 0;
}
long Recv(int, void *, unsigned long, int) { return Fail(kWouldBlock); }
long RecvFrom(int, void *, unsigned long, int, sockaddr *, unsigned int *) { return Fail(kWouldBlock); }
long Send(int, const void *, unsigned long size, int) { return static_cast<long>(size); }
long SendTo(int, const void *, unsigned long size, int, const sockaddr *, unsigned int) { return static_cast<long>(size); }
hostent * GetHostByName(const char *) { return nullptr; }
int GetAddrInfo(const char *, const char *, const addrinfo *, addrinfo ** result)
{
    if (result)
    {
        *result = nullptr;
    }
    return 2;       // EAI_AGAIN: the name could not be looked up for now
}
void FreeAddrInfo(addrinfo *) {}

// "1.2.3.4" to four bytes and back. Only IPv4 (family 2); returns as inet_pton does.
int InetPton(int family, const char * text, void * out)
{
    if (family != 2 || !text || !out)
    {
        return family == 2 ? 0 : -1;
    }
    unsigned char bytes[4];
    for (int i = 0; i < 4; i++)
    {
        if (*text < '0' || *text > '9')
        {
            return 0;
        }
        unsigned value = 0;
        while (*text >= '0' && *text <= '9')
        {
            value = value * 10 + static_cast<unsigned>(*text++ - '0');
            if (value > 255)
            {
                return 0;
            }
        }
        bytes[i] = static_cast<unsigned char>(value);
        if (i < 3 && *text++ != '.')
        {
            return 0;
        }
    }
    if (*text)
    {
        return 0;
    }
    __builtin_memcpy(out, bytes, 4);
    return 1;
}

static void Format(const unsigned char * b, char * out, unsigned int size)
{
    char text[16];
    int n = 0;
    for (int i = 0; i < 4; i++)
    {
        unsigned v = b[i];
        if (v >= 100) text[n++] = static_cast<char>('0' + v / 100);
        if (v >= 10) text[n++] = static_cast<char>('0' + v / 10 % 10);
        text[n++] = static_cast<char>('0' + v % 10);
        text[n++] = i < 3 ? '.' : '\0';
    }
    for (unsigned int i = 0; i < size; i++)
    {
        out[i] = i < static_cast<unsigned int>(n) ? text[i] : '\0';
    }
    if (size)
    {
        out[size - 1] = '\0';
    }
}

const char * InetNtop(int family, const void * address, char * out, unsigned int size)
{
    if (family != 2 || !address || !out || size < 16)
    {
        return nullptr;
    }
    Format(static_cast<const unsigned char *>(address), out, size);
    return out;
}

char * InetNtoa(in_addr address)
{
    static char text[16];
    Format(reinterpret_cast<const unsigned char *>(&address.s_addr), text, sizeof(text));
    return text;
}

}}  // namespace nn::socket

extern "C" {
unsigned short nnsocketInetHtons(unsigned short value) { return __builtin_bswap16(value); }
unsigned short nnsocketInetNtohs(unsigned short value) { return __builtin_bswap16(value); }
}
