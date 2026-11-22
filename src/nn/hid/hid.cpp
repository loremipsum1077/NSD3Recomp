// nn::hid: controllers. For now the answer to every question is "no controller is
// connected": the game starts and runs, and input arrives in milestone 4.
#include "nn/os/os_types.h"

namespace nn { namespace os {
void InitializeEvent(EventType * event, bool signaled, EventClearMode mode);
}}

namespace nn { namespace util {
template <int N, typename Tag> struct BitFlagSet {
    uint32_t storage[(N + 31) / 32];
};
}}

namespace nn { namespace hid {

struct NpadStyleTag;
typedef nn::util::BitFlagSet<32, NpadStyleTag> NpadStyleSet;

struct NpadFullKeyState;
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
// It is created unsignaled and nothing signals it yet.
void BindNpadStyleSetUpdateEvent(const unsigned int &, nn::os::SystemEventType * event, nn::os::EventClearMode mode)
{
    nn::os::InitializeEvent(&event->event, false, mode);
}

// An empty style set means nothing is connected in that slot.
NpadStyleSet GetNpadStyleSet(const unsigned int &)
{
    return NpadStyleSet{};
}

// Each returns how many states it wrote: none.
int GetNpadStates(NpadFullKeyState *, int, const unsigned int &) { return 0; }
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
