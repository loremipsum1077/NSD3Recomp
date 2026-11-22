// Stands in for the decomp's header of the same path, which specialises internal
// templates of the Switch's C++ library to reproduce the original code. On the
// host the standard float classification functions do the same job.
#pragma once
#include <cmath>
#undef isfinite
#undef isnan
using std::isfinite;
using std::isnan;
