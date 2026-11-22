// The host window and input, on SDL.
//
// Keyboard: W A S D left stick, I J K L right stick, arrow keys d-pad,
//   Space or Return = A, Escape = B, Q = Y, E = X, Tab = Plus, Backspace = Minus,
//   1 = L, 2 = R, 3 = ZL, 4 = ZR.
// Game controllers map by position: the right-hand face button is always the
// Switch's A, whatever letter is printed on it.
#include "window.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

SDL_Window * g_window;
SDL_MetalView g_metalView;
SDL_GameController * g_controller;
bool g_started;
bool g_quit;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
d3_pad_state g_state;

struct KeyButton { SDL_Scancode key; int button; };
const KeyButton kKeys[] = {
    {SDL_SCANCODE_SPACE, D3_PAD_A}, {SDL_SCANCODE_RETURN, D3_PAD_A}, {SDL_SCANCODE_ESCAPE, D3_PAD_B},
    {SDL_SCANCODE_E, D3_PAD_X}, {SDL_SCANCODE_Q, D3_PAD_Y}, {SDL_SCANCODE_TAB, D3_PAD_PLUS},
    {SDL_SCANCODE_BACKSPACE, D3_PAD_MINUS}, {SDL_SCANCODE_1, D3_PAD_L}, {SDL_SCANCODE_2, D3_PAD_R},
    {SDL_SCANCODE_3, D3_PAD_ZL}, {SDL_SCANCODE_4, D3_PAD_ZR}, {SDL_SCANCODE_LEFT, D3_PAD_LEFT},
    {SDL_SCANCODE_UP, D3_PAD_UP}, {SDL_SCANCODE_RIGHT, D3_PAD_RIGHT}, {SDL_SCANCODE_DOWN, D3_PAD_DOWN},
};

struct PadButton { SDL_GameControllerButton from; int button; };
const PadButton kPad[] = {
    {SDL_CONTROLLER_BUTTON_B, D3_PAD_A}, {SDL_CONTROLLER_BUTTON_A, D3_PAD_B},
    {SDL_CONTROLLER_BUTTON_Y, D3_PAD_X}, {SDL_CONTROLLER_BUTTON_X, D3_PAD_Y},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, D3_PAD_STICK_L}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, D3_PAD_STICK_R},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, D3_PAD_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, D3_PAD_R},
    {SDL_CONTROLLER_BUTTON_START, D3_PAD_PLUS}, {SDL_CONTROLLER_BUTTON_BACK, D3_PAD_MINUS},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, D3_PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_UP, D3_PAD_UP},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, D3_PAD_RIGHT}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, D3_PAD_DOWN},
};

int Axis(int positive, int negative)
{
    return (positive ? 32767 : 0) - (negative ? 32767 : 0);
}

int Clamp(int value)
{
    return value > 32767 ? 32767 : value < -32767 ? -32767 : value;
}

// Scripted input, for testing without a person: D3_INPUT_SCRIPT="5:A 8.5:PLUS 12:B"
// presses each named button for a quarter of a second at the given number of seconds
// after the window opened. Names: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN, and
// LSUP LSDOWN LSLEFT LSRIGHT to push the left stick. "@seconds" after a name holds it
// longer: "40:LSRIGHT@2".
int g_scriptStickX, g_scriptStickY;

uint64_t ScriptedButtons()
{
    static const struct { const char * name; int button; } kNames[] = {
        {"A", D3_PAD_A}, {"B", D3_PAD_B}, {"X", D3_PAD_X}, {"Y", D3_PAD_Y}, {"L", D3_PAD_L}, {"R", D3_PAD_R},
        {"ZL", D3_PAD_ZL}, {"ZR", D3_PAD_ZR}, {"PLUS", D3_PAD_PLUS}, {"MINUS", D3_PAD_MINUS},
        {"LEFT", D3_PAD_LEFT}, {"UP", D3_PAD_UP}, {"RIGHT", D3_PAD_RIGHT}, {"DOWN", D3_PAD_DOWN}};
    static const char * script = getenv("D3_INPUT_SCRIPT");
    static Uint32 start = SDL_GetTicks();
    if (!script)
    {
        return 0;
    }
    double now = (SDL_GetTicks() - start) / 1000.0;
    uint64_t buttons = 0;
    g_scriptStickX = g_scriptStickY = 0;
    for (const char * p = script; *p;)
    {
        char name[8] = {};
        double at = 0;
        int used = 0;
        if (sscanf(p, " %lf:%7[A-Z]%n", &at, name, &used) != 2)
        {
            break;
        }
        p += used;
        double hold = 0.25;
        int more = 0;
        if (sscanf(p, "@%lf%n", &hold, &more) == 1)
        {
            p += more;
        }
        if (now >= at && now < at + hold)
        {
            g_scriptStickX += strcmp(name, "LSRIGHT") == 0 ? 32767 : strcmp(name, "LSLEFT") == 0 ? -32767 : 0;
            g_scriptStickY += strcmp(name, "LSUP") == 0 ? 32767 : strcmp(name, "LSDOWN") == 0 ? -32767 : 0;
            for (const auto & n : kNames)
            {
                if (strcmp(n.name, name) == 0)
                {
                    buttons |= 1ull << n.button;
                }
            }
        }
    }
    return buttons;
}

void OpenFirstController()
{
    for (int i = 0; !g_controller && i < SDL_NumJoysticks(); i++)
    {
        if (SDL_IsGameController(i))
        {
            g_controller = SDL_GameControllerOpen(i);
        }
    }
}

void Sample()
{
    d3_pad_state s;
    memset(&s, 0, sizeof s);
    s.connected = 1;

    const Uint8 * keys = SDL_GetKeyboardState(nullptr);
    for (const KeyButton & k : kKeys)
    {
        if (keys[k.key])
        {
            s.buttons |= 1ull << k.button;
        }
    }
    s.leftX = Axis(keys[SDL_SCANCODE_D], keys[SDL_SCANCODE_A]);
    s.leftY = Axis(keys[SDL_SCANCODE_W], keys[SDL_SCANCODE_S]);
    s.rightX = Axis(keys[SDL_SCANCODE_L], keys[SDL_SCANCODE_J]);
    s.rightY = Axis(keys[SDL_SCANCODE_I], keys[SDL_SCANCODE_K]);

    if (g_controller)
    {
        for (const PadButton & b : kPad)
        {
            if (SDL_GameControllerGetButton(g_controller, b.from))
            {
                s.buttons |= 1ull << b.button;
            }
        }
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000)
        {
            s.buttons |= 1ull << D3_PAD_ZL;
        }
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000)
        {
            s.buttons |= 1ull << D3_PAD_ZR;
        }
        // SDL's Y axes point down; the Switch's point up.
        s.leftX = Clamp(s.leftX + SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX));
        s.leftY = Clamp(s.leftY - SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY));
        s.rightX = Clamp(s.rightX + SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX));
        s.rightY = Clamp(s.rightY - SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY));
    }

    s.buttons |= ScriptedButtons();
    s.leftX = Clamp(s.leftX + g_scriptStickX);
    s.leftY = Clamp(s.leftY + g_scriptStickY);

    pthread_mutex_lock(&g_lock);
    bool changed = s.buttons != g_state.buttons;
    g_state = s;
    pthread_mutex_unlock(&g_lock);

    // D3_INPUT_TRACE prints the buttons whenever they change, to check a mapping.
    static int trace = -1;
    if (trace < 0)
    {
        trace = getenv("D3_INPUT_TRACE") != nullptr;
    }
    if (trace && changed)
    {
        fprintf(stderr, "input: buttons %04llx  controller %s\n", static_cast<unsigned long long>(s.buttons),
                g_controller ? SDL_GameControllerName(g_controller) : "none (keyboard only)");
    }
}

}  // namespace

void d3_window_create(void)
{
    if (g_started || getenv("D3_HEADLESS"))
    {
        return;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        fprintf(stderr, "No window: %s\n", SDL_GetError());
        return;
    }
    g_started = true;
    g_window = SDL_CreateWindow("Diablo III", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_METAL);
    if (!g_window)
    {
        fprintf(stderr, "No window: %s\n", SDL_GetError());
    }
    else
    {
        g_metalView = SDL_Metal_CreateView(g_window);
    }
    OpenFirstController();
    if (getenv("D3_INPUT_TRACE"))
    {
        fprintf(stderr, "input: window %s, controller %s\n", g_window ? "open" : "not open",
                g_controller ? SDL_GameControllerName(g_controller) : "none (keyboard only)");
    }
}

void * d3_window_metal_layer(void)
{
    return g_metalView ? SDL_Metal_GetLayer(g_metalView) : nullptr;
}

int d3_window_pump(void)
{
    if (!g_started)
    {
        return 0;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_QUIT:
            g_quit = true;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            OpenFirstController();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_controller && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller)))
            {
                SDL_GameControllerClose(g_controller);
                g_controller = nullptr;
                OpenFirstController();
            }
            break;
        }
    }
    Sample();
    return g_quit;
}

void d3_input_read(struct d3_pad_state * out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_state;
    pthread_mutex_unlock(&g_lock);
}
