// nn::oe: how the console is being used (docked or handheld, focus, exit requests).
// The port presents itself as a docked console that always has focus.
#include <unistd.h>
#include "host/window.h"
#include "nn/hid/hid_host.h"
#include "nn/nn_types.h"

#include <pthread.h>

#include <stdlib.h>
#include <string.h>

namespace nn { namespace settings {

enum Language {
    Language_Japanese, Language_AmericanEnglish, Language_French, Language_German, Language_Italian,
    Language_Spanish, Language_Chinese, Language_Korean, Language_Dutch, Language_Portuguese,
    Language_Russian, Language_Taiwanese, Language_BritishEnglish, Language_CanadianFrench,
    Language_LatinAmericanSpanish, Language_SimplifiedChinese, Language_TraditionalChinese,
    Language_Count
};

struct LanguageCode {
    char string[8];
    static LanguageCode Make(Language language);
};

static const char * const kCodes[Language_Count] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419",
    "zh-Hans", "zh-Hant"};

LanguageCode LanguageCode::Make(Language language)
{
    LanguageCode code;
    memset(&code, 0, sizeof(code));
    if (language >= 0 && language < Language_Count)
    {
        strncpy(code.string, kCodes[language], sizeof(code.string) - 1);
    }
    return code;
}

bool operator==(const LanguageCode & left, const LanguageCode & right)
{
    return strncmp(left.string, right.string, sizeof(left.string)) == 0;
}

// Newer SDKs (Diablo III 2.7.7) compare a code with a language directly.
bool operator==(const LanguageCode & left, const Language & right)
{
    return left == LanguageCode::Make(right);
}

}}  // namespace nn::settings

namespace nn { namespace oe {

enum OperationMode { OperationMode_Handheld = 0, OperationMode_Console = 1 };
enum PerformanceMode { PerformanceMode_Invalid = -1, PerformanceMode_Normal = 0, PerformanceMode_Boost = 1 };
enum FocusState { FocusState_InFocus = 1, FocusState_OutOfFocus = 2, FocusState_Background = 3 };
enum CpuBoostMode { CpuBoostMode_Normal = 0, CpuBoostMode_FastLoad = 1 };

// 2.7.7 raises the CPU clock during loads. A desktop has nothing to switch.
void SetCpuBoostMode(CpuBoostMode) {}

enum FocusHandlingMode { FocusHandlingMode_Suspend, FocusHandlingMode_Notify, FocusHandlingMode_SuspendAndNotify, FocusHandlingMode_InFocusOnly };

OperationMode GetOperationMode()
{
    return OperationMode_Console;
}

PerformanceMode GetPerformanceMode()
{
    return PerformanceMode_Boost;
}

void SetPerformanceConfiguration(PerformanceMode, int)
{
}

FocusState GetCurrentFocusState()
{
    return FocusState_InFocus;
}

// The language the game should use. Set D3_LANGUAGE to one of the Switch's codes
// (en-US, fr, de, ja ...); American English otherwise.
settings::LanguageCode GetDesiredLanguage()
{
    settings::LanguageCode code = settings::LanguageCode::Make(settings::Language_AmericanEnglish);
    const char * wanted = getenv("D3_LANGUAGE");
    for (int i = 0; wanted && i < settings::Language_Count; i++)
    {
        if (strcmp(wanted, settings::kCodes[i]) == 0)
        {
            code = settings::LanguageCode::Make(static_cast<settings::Language>(i));
        }
    }
    return code;
}

// The game asks for system messages once per frame from its main loop, which makes
// this the place where the host's window and input are serviced. The only message
// delivered so far is the exit request, sent once when the window is closed.
bool TryPopNotificationMessage(unsigned int * message)
{
    static bool exitSent;
    if (!pthread_main_np())
    {
        return false;
    }
    int quit = d3_window_pump();
    d3_hid_poll();
    if (quit && !exitSent)
    {
        exitSent = true;
        // The game is told, and given a few seconds to save and leave by itself.
        // Its own shutdown does not yet finish here, so after that the process ends.
        pthread_t watchdog;
        pthread_create(&watchdog, nullptr, [](void *) -> void * {
            sleep(3);
            _exit(0);
        }, nullptr);
        pthread_detach(watchdog);
        *message = 4;       // MessageExitRequest
        return true;
    }
    return false;
}

void FinishStartupLogo() {}
void SetFocusHandlingMode(FocusHandlingMode) {}
void SetResumeNotificationEnabled(bool) {}
void SetOperationModeChangedNotificationEnabled(bool) {}
void SetPerformanceModeChangedNotificationEnabled(bool) {}
void EnterExitRequestHandlingSection() {}
void LeaveExitRequestHandlingSection() {}

}}  // namespace nn::oe
