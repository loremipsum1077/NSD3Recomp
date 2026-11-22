// nn::hid: controllers.
//
// The port presents one controller: a Pro Controller in the first player slot, fed
// by the host's keyboard and first game controller (src/host/window.cpp). It is
// connected whenever the window exists. Motion sensors and rumble are absent.
#include "hid_host.h"

#include "host/window.h"
#include "nn/os/os_types.h"

namespace nn { namespace os {
void InitializeEvent(EventType * event, bool signaled, EventClearMode mode);
void SignalEvent(EventType * event);
}}

namespace {
const unsigned int kFirstPlayer = 0;        // NpadId::No1
const uint32_t kStyleFullKey = 1u << 0;     // Pro Controller
const uint32_t kAttributeConnected = 1u << 0;

// Events the game asked to be told about controller changes on. Plain pointers and
// an integer: usable before this file's own start-up has run.
nn::os::SystemEventType * g_styleEvents[16];
int g_styleEventCount;
int g_wasConnected;
int64_t g_samplingNumber;
}

// Called once per frame from the main thread, after input has been sampled.
void d3_hid_poll(void)
{
    d3_pad_state pad;
    d3_input_read(&pad);
    if (pad.connected != g_wasConnected)
    {
        g_wasConnected = pad.connected;
        for (int i = 0; i < g_styleEventCount; i++)
        {
            nn::os::SignalEvent(&g_styleEvents[i]->event);
        }
    }
}

namespace nn { namespace util {
template <int N, typename Tag> struct BitFlagSet {
    uint32_t storage[(N + 31) / 32];
};
}}

namespace nn { namespace hid {

struct NpadStyleTag;
typedef nn::util::BitFlagSet<32, NpadStyleTag> NpadStyleSet;

struct NpadFullKeyState {
    int64_t samplingNumber;
    uint64_t buttons;
    int32_t leftX, leftY;
    int32_t rightX, rightY;
    uint32_t attributes;
};
struct NpadHandheldState;
struct NpadJoyDualState;
struct NpadJoyLeftState;
struct NpadJoyRightState;
struct SixAxisSensorState;
struct SixAxisSensorHandle;
struct VibrationDeviceHandle;
struct VibrationDeviceInfo;
struct VibrationValue;
struct ControllerSupportArg;
enum NpadJoyHoldType { NpadJoyHoldType_Vertical, NpadJoyHoldType_Horizontal };

void InitializeNpad() {}
void SetSupportedNpadIdType(const unsigned int *, unsigned long) {}
void SetSupportedNpadStyleSet(NpadStyleSet) {}
void SetNpadJoyHoldType(NpadJoyHoldType) {}

// The game waits on this event to learn that a controller was connected or removed.
// It starts signalled so the game looks once at start-up.
void BindNpadStyleSetUpdateEvent(const unsigned int &, nn::os::SystemEventType * event, nn::os::EventClearMode mode)
{
    nn::os::InitializeEvent(&event->event, true, mode);
    if (g_styleEventCount < 16)
    {
        g_styleEvents[g_styleEventCount++] = event;
    }
}

// An empty style set means nothing is connected in that slot.
NpadStyleSet GetNpadStyleSet(const unsigned int & id)
{
    d3_pad_state pad;
    d3_input_read(&pad);
    NpadStyleSet styles{};
    if (id == kFirstPlayer && pad.connected)
    {
        styles.storage[0] = kStyleFullKey;
    }
    return styles;
}

// Each returns how many states it wrote, newest first. Only the newest is offered.
int GetNpadStates(NpadFullKeyState * out, int count, const unsigned int & id)
{
    d3_pad_state pad;
    d3_input_read(&pad);
    if (id != kFirstPlayer || !pad.connected || count < 1)
    {
        return 0;
    }
    out->samplingNumber = __atomic_add_fetch(&g_samplingNumber, 1, __ATOMIC_RELAXED);
    out->buttons = pad.buttons;
    out->leftX = pad.leftX;
    out->leftY = pad.leftY;
    out->rightX = pad.rightX;
    out->rightY = pad.rightY;
    out->attributes = kAttributeConnected;
    return 1;
}
int GetNpadStates(NpadHandheldState *, int, const unsigned int &) { return 0; }
int GetNpadStates(NpadJoyDualState *, int, const unsigned int &) { return 0; }
int GetNpadStates(NpadJoyLeftState *, int, const unsigned int &) { return 0; }
int GetNpadStates(NpadJoyRightState *, int, const unsigned int &) { return 0; }

int GetSixAxisSensorHandles(SixAxisSensorHandle *, int, const unsigned int &, NpadStyleSet) { return 0; }
void StartSixAxisSensor(const SixAxisSensorHandle &) {}
void StopSixAxisSensor(const SixAxisSensorHandle &) {}
int GetSixAxisSensorStates(SixAxisSensorState *, int, const SixAxisSensorHandle &) { return 0; }

int GetVibrationDeviceHandles(VibrationDeviceHandle *, int, const unsigned int &, NpadStyleSet) { return 0; }
void InitializeVibrationDevice(const VibrationDeviceHandle &) {}
void GetVibrationDeviceInfo(VibrationDeviceInfo *, const VibrationDeviceHandle &) {}
void SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &) {}

// System screens for pairing controllers; there is nothing to show.
Result ShowControllerSupport(const ControllerSupportArg &) { return ResultSuccess(); }
void ShowControllerStrapGuide() {}

}}  // namespace nn::hid
