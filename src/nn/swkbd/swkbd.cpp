// nn::swkbd: the Switch's on-screen keyboard.
//
// There is no text entry yet. When the game asks for text (a hero's name), it is
// answered at once with D3_HERO_NAME if that is set, and "Hero" otherwise. A real
// text field in the host window belongs to milestone 8.
#include "nn/nn_types.h"

#include <stdlib.h>
#include <string.h>

namespace nn { namespace swkbd {

struct KeyboardConfig;
struct ShowKeyboardArg;
enum Preset { Preset_Default };

struct String {
    void * ptr;
    size_t bufSize;
};

// The game fills in a settings structure through these. Nothing reads it here.
void MakePreset(KeyboardConfig *, Preset) {}
void SetHeaderTextUtf8(KeyboardConfig *, const char *) {}
void SetSubTextUtf8(KeyboardConfig *, const char *) {}
void SetGuideTextUtf8(KeyboardConfig *, const char *) {}
void SetInitialTextUtf8(ShowKeyboardArg *, const char *) {}

unsigned long GetRequiredWorkBufferSize(bool) { return 0x1000; }
unsigned long GetRequiredStringBufferSize() { return 0x800; }

// The game asks for UTF-8. Success means the player confirmed the text.
Result ShowKeyboard(String * out, const ShowKeyboardArg &)
{
    const char * name = getenv("D3_HERO_NAME");
    if (!name || !*name)
    {
        name = "Hero";
    }
    if (out->ptr && out->bufSize)
    {
        strncpy(static_cast<char *>(out->ptr), name, out->bufSize - 1);
        static_cast<char *>(out->ptr)[out->bufSize - 1] = 0;
    }
    return ResultSuccess();
}

}}  // namespace nn::swkbd
