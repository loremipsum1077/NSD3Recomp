// Types shared by the platform layer's SDK functions.
//
// These are the platform layer's own declarations, not the decomp's. Only two
// things must agree with the game: the names (they are part of each function's
// linker name) and the size of every object the game allocates, which is the
// space these layouts have to fit in. The game never reads the fields itself.
// Parameters must be spelled with the same basic types as the Switch build
// (`unsigned long`, not uint64_t) for the linker names to match.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nn {

struct Result {          // 4 bytes, zero is success
    uint32_t value;
};
inline Result ResultSuccess() { return Result{0}; }

class TimeSpan {         // nanoseconds
public:
    int64_t ns;
};

namespace account {

struct Uid {             // identifies a user profile; all zero is "no user"
    uint64_t data[2];
};

struct Nickname { char name[33]; };
struct NetworkServiceAccountId { uint64_t id; };

}  // namespace account

}  // namespace nn
