// The Switch's own screens that the game can ask the console to show: the error
// dialog, the built-in web page viewer (used for "What's New"), and the shop. There
// is no console here, so each is reported on the terminal and returns as if the
// player had closed it at once. Also nn::nsd, which maps the names of online
// services to servers: nothing resolves, because the port does not go online.
#include "nn/nn_types.h"

#include <stdio.h>
#include <string.h>

namespace nn {

struct ApplicationId { uint64_t value; };
namespace account { struct UserHandle; }

namespace err {

// A Switch error code reads 2MMM-DDDD: the result's module and description.
void ShowError(Result result)
{
    fprintf(stderr, "The game asked the console to show error %04u-%04u (0x%x).\n", 2000 + (result.value & 0x1ff), (result.value >> 9) & 0x1fff,
            result.value);
}

}  // namespace err

namespace web {

class ShowWebPageArg {      // 0x2000 bytes on the Switch; the address is kept at the start
public:
    explicit ShowWebPageArg(const char * url);
    char data[0x2000];
};

class WebPageReturnValue {  // 0x1010 bytes on the Switch
public:
    WebPageReturnValue();
    char data[0x1010];
};

ShowWebPageArg::ShowWebPageArg(const char * url)
{
    memset(data, 0, sizeof(data));
    if (url)
    {
        strncpy(data, url, sizeof(data) - 1);
    }
}

WebPageReturnValue::WebPageReturnValue()
{
    memset(data, 0, sizeof(data));
}

Result ShowWebPage(WebPageReturnValue *, const ShowWebPageArg & arg)
{
    fprintf(stderr, "The game asked the console to show a web page: %s\n", arg.data);
    return ResultSuccess();
}

}  // namespace web

namespace ec {

Result ShowShopAddOnContentList(ApplicationId, const account::UserHandle &)
{
    fprintf(stderr, "The game asked the console to open the shop.\n");
    return ResultSuccess();
}

}  // namespace ec

namespace nsd {

struct Fqdn;

Result ResolveEx(Fqdn *, const Fqdn &)
{
    return Result{(1u << 9) | 141};     // a failure in the nsd module: nothing resolves
}

}  // namespace nsd

}  // namespace nn
