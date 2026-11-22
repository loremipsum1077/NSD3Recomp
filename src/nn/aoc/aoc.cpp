// nn::aoc: add-on content. The port has none.
#include "nn/os/os_types.h"

namespace nn { namespace os {
void InitializeEvent(EventType * event, bool signaled, EventClearMode mode);
class SystemEvent;      // the SDK's wrapper class; it holds one SystemEventType
}}

namespace nn { namespace aoc {

int CountAddOnContent()
{
    return 0;
}

int ListAddOnContent(int *, int, int)
{
    return 0;
}

// Signalled when add-on content is installed while the game runs: never.
void GetAddOnContentListChangedEvent(nn::os::SystemEvent * event)
{
    nn::os::InitializeEvent(&reinterpret_cast<nn::os::SystemEventType *>(event)->event, false, nn::os::EventClearMode_ManualClear);
}

}}  // namespace nn::aoc
