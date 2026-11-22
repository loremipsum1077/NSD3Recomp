// nn::account: user profiles. The port has exactly one local user, with no Nintendo
// network account. D3_PLAYER_NAME sets the nickname.
#include "nn/nn_types.h"

#include <stdlib.h>
#include <string.h>

namespace {

const nn::account::Uid kLocalUser = {{0x0000000000000001ull, 0x4433000000000000ull}};

// What the network-account calls answer: module 124 (account), "not available".
nn::Result NoNetworkAccount()
{
    return nn::Result{124u | 100u << 9};
}

}  // namespace

namespace nn { namespace account {

struct UserHandle {
    uint64_t data[2];
    uintptr_t context;
};

struct UserSelectionSettings;

class AsyncContext {
public:
    AsyncContext();
    ~AsyncContext();
    Result HasDone(bool * done);
    Result GetResult();

private:
    uint64_t m_storage[4];
};

AsyncContext::AsyncContext() : m_storage{}
{
}

AsyncContext::~AsyncContext()
{
}

// Nothing here is ever started, so it is always finished, with a failure.
Result AsyncContext::HasDone(bool * done)
{
    *done = true;
    return ResultSuccess();
}

Result AsyncContext::GetResult()
{
    return NoNetworkAccount();
}

void Initialize()
{
}

Result ListAllUsers(int * count, Uid * users, int capacity)
{
    *count = 0;
    if (capacity > 0)
    {
        users[0] = kLocalUser;
        *count = 1;
    }
    return ResultSuccess();
}

Result GetNickname(Nickname * out, const Uid &)
{
    const char * name = getenv("D3_PLAYER_NAME");
    memset(out, 0, sizeof(*out));
    strncpy(out->name, name && *name ? name : "Player", sizeof(out->name) - 1);
    return ResultSuccess();
}

Result OpenUser(UserHandle * out, const Uid & user)
{
    out->data[0] = user.data[0];
    out->data[1] = user.data[1];
    out->context = 0;
    return ResultSuccess();
}

Result OpenPreselectedUser(UserHandle * out)
{
    return OpenUser(out, kLocalUser);
}

void CloseUser(const UserHandle &)
{
}

Result GetUserId(Uid * out, const UserHandle & handle)
{
    out->data[0] = handle.data[0];
    out->data[1] = handle.data[1];
    return ResultSuccess();
}

// The Switch would show its user picker; with one user there is nothing to pick.
Result ShowUserSelector(Uid * out, const UserSelectionSettings &)
{
    *out = kLocalUser;
    return ResultSuccess();
}

// ---- Nintendo network account: never available ----

Result IsNetworkServiceAccountAvailable(bool * available, const UserHandle &)
{
    *available = false;
    return ResultSuccess();
}

Result GetNetworkServiceAccountId(NetworkServiceAccountId *, const UserHandle &) { return NoNetworkAccount(); }
Result EnsureNetworkServiceAccountAvailable(const UserHandle &) { return NoNetworkAccount(); }
Result LoadNetworkServiceAccountIdTokenCache(unsigned long *, char *, unsigned long, const UserHandle &) { return NoNetworkAccount(); }
Result ShowLicenseRequirementsForNetworkService(const UserHandle &) { return NoNetworkAccount(); }
Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *, const UserHandle &) { return NoNetworkAccount(); }

}}  // namespace nn::account
