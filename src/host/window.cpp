// The host window and input, on SDL.
//
// Keyboard: W A S D left stick, I J K L right stick, arrow keys d-pad,
//   Space or Return = A, Escape = B, Q = Y, E = X, Tab = Plus, Backspace = Minus,
//   1 = L, 2 = R, 3 = ZL, 4 = ZR.
//   F1, F2 and F3 are the Cheats mod's hotkeys (d3_hotkeys_take).
// Mouse: queued for the Mouse mod (d3_mouse_take), which hands it to the game.
// Game controllers map by position: the right-hand face button is always the
// Switch's A, whatever letter is printed on it.
#include "window.h"

#include <SDL.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace {

SDL_Window * g_window;
SDL_MetalView g_metalView;
SDL_GameController * g_controller;
bool g_started;
bool g_quit;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
d3_pad_state g_state;
int g_hotkeys;  // under g_lock

const int kMouseQueue = 256;
d3_mouse_event g_mouse[kMouseQueue];  // under g_lock
int g_mouseCount;

void QueueMouseAt(int type, int button, int wheel, float x, float y)
{
    d3_mouse_event e;
    e.type = type;
    e.button = button;
    e.wheel = wheel;
    e.x = x;
    e.y = y;
    pthread_mutex_lock(&g_lock);
    if (g_mouseCount == kMouseQueue)
    {
        memmove(g_mouse, g_mouse + 1, sizeof g_mouse[0] * (kMouseQueue - 1));
        g_mouseCount--;
    }
    g_mouse[g_mouseCount++] = e;
    pthread_mutex_unlock(&g_lock);
}

void QueueMouse(int type, int button, int wheel, int x, int y)
{
    if (getenv("D3_MOUSE_SCRIPT"))
    {
        return;  // a scripted run ignores the real pointer
    }
    int width = 1, height = 1;
    if (g_window)
    {
        SDL_GetWindowSize(g_window, &width, &height);
    }
    QueueMouseAt(type, button, wheel, width > 0 ? static_cast<float>(x) / width : 0,
                 height > 0 ? static_cast<float>(y) / height : 0);
}

// Scripted mouse, for testing without a person: D3_MOUSE_SCRIPT="40:left:0.5,0.4
// 45:right:0.6,0.5@2 50:move:0.3,0.3" moves to the given fraction of the window at the
// given number of seconds after the window opened and, for left/right/middle, presses
// that button there and releases it a tenth of a second later ("@seconds" holds longer).
void ScriptedMouse()
{
    struct Step { double at; int type, button; float x, y; };
    static std::vector<Step> * steps;
    static size_t next;
    static Uint32 start = SDL_GetTicks();
    if (!steps)
    {
        steps = new std::vector<Step>;
        const char * script = getenv("D3_MOUSE_SCRIPT");
        for (const char * p = script ? script : ""; *p;)
        {
            double at = 0, hold = 0.1;
            char action[8] = {};
            float x = 0, y = 0;
            int used = 0, more = 0;
            if (sscanf(p, " %lf:%7[a-z]:%f,%f%n", &at, action, &x, &y, &used) != 4)
            {
                break;
            }
            p += used;
            if (sscanf(p, "@%lf%n", &hold, &more) == 1)
            {
                p += more;
            }
            int button = strcmp(action, "right") == 0 ? D3_MOUSE_RIGHT : strcmp(action, "middle") == 0 ? D3_MOUSE_MIDDLE : D3_MOUSE_LEFT;
            steps->push_back({at, D3_MOUSE_MOVE, 0, x, y});
            if (strcmp(action, "move") != 0)
            {
                steps->push_back({at, D3_MOUSE_DOWN, button, x, y});
                steps->push_back({at + hold, D3_MOUSE_UP, button, x, y});
            }
        }
        std::stable_sort(steps->begin(), steps->end(), [](const Step & a, const Step & b) { return a.at < b.at; });
    }
    double now = (SDL_GetTicks() - start) / 1000.0;
    for (; next < steps->size() && (*steps)[next].at <= now; next++)
    {
        const Step & s = (*steps)[next];
        QueueMouseAt(s.type, s.button, 0, s.x, s.y);
    }
}

// d3_pad_tap holds a pad button down until this tick (0 for none); d3_mouse_stick tilts
// the left stick (the Mouse mod walks this way).
Uint32 g_tapUntil[64];
int g_mouseStickX, g_mouseStickY;

int MouseButtonOf(Uint8 sdlButton)
{
    return sdlButton == SDL_BUTTON_RIGHT ? D3_MOUSE_RIGHT : sdlButton == SDL_BUTTON_MIDDLE ? D3_MOUSE_MIDDLE : D3_MOUSE_LEFT;
}

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
// longer: "40:LSRIGHT@2". F1-F3 and F5-F8 press that hotkey once.
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
    static bool fired[64];
    int entry = 0;
    for (const char * p = script; *p; entry++)
    {
        char name[8] = {};
        double at = 0;
        int used = 0;
        if (sscanf(p, " %lf:%7[A-Z0-9]%n", &at, name, &used) != 2)
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
        if (name[0] == 'F' && name[1] >= '1' && name[1] <= '8' && name[2] == 0)
        {
            static const int kKeys[] = {D3_HOTKEY_LEVEL_MAX, D3_HOTKEY_PARAGON, D3_HOTKEY_GOLD, 0,
                                        D3_HOTKEY_CAMERA, D3_HOTKEY_TARGET_NAME, D3_HOTKEY_STICK_VIEW, D3_HOTKEY_DESIGNER};
            if (now >= at && entry < 64 && !fired[entry])
            {
                fired[entry] = true;
                pthread_mutex_lock(&g_lock);
                g_hotkeys |= kKeys[name[1] - '1'];
                pthread_mutex_unlock(&g_lock);
            }
            continue;
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
    ScriptedMouse();
    pthread_mutex_lock(&g_lock);
    for (int b = 0; b < 64; b++)
    {
        if (g_tapUntil[b] && SDL_TICKS_PASSED(g_tapUntil[b], SDL_GetTicks()) == SDL_FALSE)
        {
            s.buttons |= 1ull << b;
        }
    }
    s.leftX = Clamp(s.leftX + g_mouseStickX);
    s.leftY = Clamp(s.leftY + g_mouseStickY);
    pthread_mutex_unlock(&g_lock);
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
        case SDL_KEYDOWN:
            if (!e.key.repeat)
            {
                int key = e.key.keysym.scancode == SDL_SCANCODE_F1   ? D3_HOTKEY_LEVEL_MAX
                          : e.key.keysym.scancode == SDL_SCANCODE_F2 ? D3_HOTKEY_PARAGON
                          : e.key.keysym.scancode == SDL_SCANCODE_F3 ? D3_HOTKEY_GOLD
                          : e.key.keysym.scancode == SDL_SCANCODE_F5 ? D3_HOTKEY_CAMERA
                          : e.key.keysym.scancode == SDL_SCANCODE_F6 ? D3_HOTKEY_TARGET_NAME
                          : e.key.keysym.scancode == SDL_SCANCODE_F7 ? D3_HOTKEY_STICK_VIEW
                          : e.key.keysym.scancode == SDL_SCANCODE_F8 ? D3_HOTKEY_DESIGNER
                                                                     : 0;
                if (key)
                {
                    pthread_mutex_lock(&g_lock);
                    g_hotkeys |= key;
                    pthread_mutex_unlock(&g_lock);
                }
            }
            break;
        case SDL_MOUSEMOTION:
            QueueMouse(D3_MOUSE_MOVE, 0, 0, e.motion.x, e.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_MIDDLE)
            {
                QueueMouse(e.type == SDL_MOUSEBUTTONDOWN ? D3_MOUSE_DOWN : D3_MOUSE_UP, MouseButtonOf(e.button.button), 0,
                           e.button.x, e.button.y);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (e.wheel.y)
            {
                int x = 0, y = 0;
                SDL_GetMouseState(&x, &y);
                QueueMouse(D3_MOUSE_WHEEL, 0, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y, x, y);
            }
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

void d3_pad_tap(int button, int milliseconds)
{
    if (button < 0 || button >= 64)
    {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_tapUntil[button] = SDL_GetTicks() + static_cast<Uint32>(milliseconds) | 1;
    pthread_mutex_unlock(&g_lock);
}

void d3_mouse_stick(int x, int y)
{
    pthread_mutex_lock(&g_lock);
    g_mouseStickX = x;
    g_mouseStickY = y;
    pthread_mutex_unlock(&g_lock);
}

int d3_mouse_take(struct d3_mouse_event * out, int max)
{
    pthread_mutex_lock(&g_lock);
    int count = g_mouseCount < max ? g_mouseCount : max;
    memcpy(out, g_mouse, sizeof g_mouse[0] * count);
    memmove(g_mouse, g_mouse + count, sizeof g_mouse[0] * (g_mouseCount - count));
    g_mouseCount -= count;
    pthread_mutex_unlock(&g_lock);
    return count;
}

int d3_hotkeys_take_mask(int mask)
{
    pthread_mutex_lock(&g_lock);
    int keys = g_hotkeys & mask;
    g_hotkeys &= ~mask;
    pthread_mutex_unlock(&g_lock);
    return keys;
}

int d3_hotkeys_take(void)
{
    return d3_hotkeys_take_mask(D3_HOTKEY_LEVEL_MAX | D3_HOTKEY_PARAGON | D3_HOTKEY_GOLD);
}

void d3_input_read(struct d3_pad_state * out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_state;
    pthread_mutex_unlock(&g_lock);
}
