// nn::ldn (local wireless play). Only the address helpers are real; the game's
// headers build constant addresses with them while the program starts.
#include <stdint.h>

namespace nn { namespace ldn {

struct Ipv4Address { uint32_t raw; };

Ipv4Address MakeIpv4Address(unsigned char a, unsigned char b, unsigned char c, unsigned char d)
{
    return Ipv4Address{static_cast<uint32_t>(a) << 24 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(c) << 8 | d};
}

bool operator==(Ipv4Address left, Ipv4Address right)
{
    return left.raw == right.raw;
}

}}  // namespace nn::ldn
