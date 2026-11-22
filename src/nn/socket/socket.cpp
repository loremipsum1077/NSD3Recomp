// nn::socket. Only start-up, shutdown and the byte-order helpers are real so far;
// everything that would touch the network is still a stand-in (unimplemented.c).
#include "nn/nn_types.h"

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

extern "C" {
unsigned short nnsocketInetHtons(unsigned short value) { return __builtin_bswap16(value); }
unsigned short nnsocketInetNtohs(unsigned short value) { return __builtin_bswap16(value); }
}
