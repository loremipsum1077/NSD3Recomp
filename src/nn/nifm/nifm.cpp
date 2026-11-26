// nn::nifm: the console's network connection. The port reports that there is none,
// which keeps the game in its offline paths.
#include "nn/nn_types.h"

struct in_addr;

namespace {
// Module 110 (nifm), "not connected".
nn::Result NotConnected() { return nn::Result{110u | 1u << 9}; }
}

namespace nn { namespace nifm {

struct ProxySetting;

Result Initialize() { return ResultSuccess(); }
void SubmitNetworkRequest() {}
void SubmitNetworkRequestAndWait() {}
void CancelNetworkRequest() {}
bool IsNetworkRequestOnHold() { return false; }
bool IsNetworkAvailable() { return false; }
Result HandleNetworkRequestResult() { return NotConnected(); }
// 2.7.7: shows the system's network error dialog. Like the other console dialogs, nothing.
void HandleNetworkRequestErrorResult() {}
Result GetCurrentPrimaryIpAddress(in_addr *) { return NotConnected(); }
Result GetCurrentIpConfigInfo(in_addr *, in_addr *, in_addr *, in_addr *, in_addr *) { return NotConnected(); }
Result GetCurrentProxySetting(ProxySetting *) { return NotConnected(); }

}}  // namespace nn::nifm
