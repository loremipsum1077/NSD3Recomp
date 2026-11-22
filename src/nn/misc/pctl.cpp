// nn::pctl: parental controls. Nothing is restricted.
#include "nn/nn_types.h"

namespace nn { namespace pctl {

Result CheckFreeCommunicationPermission(bool)
{
    return ResultSuccess();
}

}}  // namespace nn::pctl
