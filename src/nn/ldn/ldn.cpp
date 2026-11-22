// nn::ldn: local wireless play. Not supported: the address helpers are real (the
// game's headers build constant addresses with them while the program starts), and
// everything else reports that the wireless device is unavailable.
#include "nn/nn_types.h"

namespace {
// Module 203 (ldn), description 16: the device is not available.
nn::Result Unavailable() { return nn::Result{203u | 16u << 9}; }
}

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

struct SubnetMask;
struct NetworkInfo;
struct NodeLatestUpdate;
struct NetworkConfig;
struct SecurityConfig;
struct UserConfig;
struct ScanFilter;
enum ConnectOption { ConnectOption_None };
enum State { State_None = 0 };

Result Initialize() { return Unavailable(); }
void Finalize() {}
State GetState() { return State_None; }
Result OpenAccessPoint() { return Unavailable(); }
Result CloseAccessPoint() { return Unavailable(); }
Result CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &) { return Unavailable(); }
Result DestroyNetwork() { return Unavailable(); }
Result OpenStation() { return Unavailable(); }
Result CloseStation() { return Unavailable(); }
Result Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int, ConnectOption) { return Unavailable(); }
Result Disconnect() { return Unavailable(); }
Result GetIpv4Address(Ipv4Address *, SubnetMask *) { return Unavailable(); }
Result GetNetworkInfo(NetworkInfo *, NodeLatestUpdate *, int) { return Unavailable(); }
Result SetAdvertiseData(const void *, unsigned long) { return Unavailable(); }

Result Scan(NetworkInfo *, int * count, int, const ScanFilter &, int)
{
    *count = 0;
    return Unavailable();
}

}}  // namespace nn::ldn
