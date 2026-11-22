// nn::nfp: amiibo. There is no reader, so no device is ever found.
#include "nn/os/os_types.h"

#include <string.h>

namespace nn { namespace os {
void InitializeEvent(EventType * event, bool signaled, EventClearMode mode);
}}

namespace {
// Module 115 (nfp), description 64: no NFC device.
nn::Result NoDevice() { return nn::Result{115u | 64u << 9}; }

void NeverSignalled(nn::os::SystemEventType * event)
{
    memset(event, 0, sizeof(*event));
    nn::os::InitializeEvent(&event->event, false, nn::os::EventClearMode_ManualClear);
}
}

namespace nn { namespace nfp {

struct DeviceHandle;
struct TagInfo;
struct RegisterInfo;
struct CommonInfo;
struct ModelInfo;
struct ApplicationAreaCreateInfo;
struct AmiiboSettingsStartParam;
enum ModelType { ModelType_Amiibo };
enum DeviceState { DeviceState_Init = 0 };

void Initialize() {}
void Finalize() {}

Result ListDevices(DeviceHandle *, int * count, int)
{
    *count = 0;
    return NoDevice();
}

void AttachAvailabilityChangeEvent(os::SystemEventType * event) { NeverSignalled(event); }

Result AttachActivateEvent(os::SystemEventType * event, const DeviceHandle &)
{
    NeverSignalled(event);
    return NoDevice();
}

Result AttachDeactivateEvent(os::SystemEventType * event, const DeviceHandle &)
{
    NeverSignalled(event);
    return NoDevice();
}

DeviceState GetDeviceState(const DeviceHandle &) { return DeviceState_Init; }
Result GetNpadId(unsigned int *, const DeviceHandle &) { return NoDevice(); }
Result StartDetection(const DeviceHandle &) { return NoDevice(); }
Result StopDetection(const DeviceHandle &) { return NoDevice(); }
Result Mount(const DeviceHandle &, ModelType) { return NoDevice(); }
Result Unmount(const DeviceHandle &) { return NoDevice(); }
Result CreateApplicationArea(const DeviceHandle &, const ApplicationAreaCreateInfo &) { return NoDevice(); }
Result OpenApplicationArea(const DeviceHandle &, unsigned int) { return NoDevice(); }
Result GetApplicationArea(void *, unsigned long *, const DeviceHandle &, unsigned long) { return NoDevice(); }
Result SetApplicationArea(const DeviceHandle &, const void *, unsigned long) { return NoDevice(); }
Result Flush(const DeviceHandle &) { return NoDevice(); }
Result Restore(const DeviceHandle &) { return NoDevice(); }
Result GetTagInfo(TagInfo *, const DeviceHandle &) { return NoDevice(); }
Result GetRegisterInfo(RegisterInfo *, const DeviceHandle &) { return NoDevice(); }
Result GetCommonInfo(CommonInfo *, const DeviceHandle &) { return NoDevice(); }
Result GetModelInfo(ModelInfo *, const DeviceHandle &) { return NoDevice(); }
Result StartNicknameAndOwnerSettings(DeviceHandle *, bool *, RegisterInfo *, const AmiiboSettingsStartParam &, const TagInfo &) { return NoDevice(); }
Result StartGameDataEraser(DeviceHandle *, const AmiiboSettingsStartParam &, const TagInfo &) { return NoDevice(); }
Result StartRestorer(DeviceHandle *, const AmiiboSettingsStartParam &, const TagInfo &) { return NoDevice(); }

}}  // namespace nn::nfp
