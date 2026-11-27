// Developer starts: ways Blizzard's own builds could start the game that the Switch release
// still has the code for.
//
// D3_MOD_LOAD_WORLD=<world>  skip the menus and start a local game in that world, named by its
//     SNO name. The decomp's MainNX64.cpp still has the loader: when the name is set it makes
//     a local developer account with a new hero of each class and puts the party in that world
//     (Console::Online::CreateDummySetup). An unknown name falls back to the usual menus.
//     Worlds that ship and are not reached in normal play: "X1_TEST_ChallengeTestWorld" and
//     "Axe FlatWorld". Any other world name works too. D3_LOAD_WORLD, when starting the game,
//     overrides it (empty for the menus).
// D3_MOD_BLIZZCON=1  the BlizzCon show-floor demo, as the -blizzcon argument starts it: an
//     offline party of level-70 demo heroes, the title screen goes straight to the main menu,
//     and the game starts in a Greater Rift. The demo's title screen loaded fewer UI files
//     than 2.7.7's game screens need (the leaderboard inspect screen's), so DecompSources.cmake
//     has it load the usual set.
//
// D3_MOD_DEV_TOOLS=1  hotkeys for developer views the game still has:
//     F5  the screenshot camera: a fixed camera looking at the hero from the front, as the
//         game's own (unsent) SMSG_SET_SCREENSHOT_VIEW sets it; F5 again goes back to the
//         normal camera;
//     F6  the current target's internal name, at the top of the screen (Console::
//         ToggleTargetName, whose caller the release dropped);
//     F7  the controller view: the left stick's position, magnitude and dead zone
//         (Console::ToggleControllerDebugView, likewise).
//     Both views are drawn by the game every frame already; the keys only switch them.
//     F8  the designer menu (designer_menu.inc).
//     F9  a free camera. It starts where the game's camera is and the controller flies it
//         instead of moving the hero: the left stick moves along the view and sideways, the
//         right stick turns, R or ZR rises and L or ZL sinks, and holding A goes four times
//         as fast. F9 again gives the controller back and returns to the normal camera.
//         Only what the game has loaded around the hero is drawn, so far away there is
//         nothing to see. The camera uses the game's static view (CS_STATIC_VIEW, which
//         nothing else updates) and moves it with CinematographyCutTo every input update.
//
// The decomp's MainNX64.cpp is patched to call D3ModDevStartup() once, after the command line
// is parsed, and with the tools on, InputNX64.cpp to call D3ModDevToolsUpdate() once per input
// update, on the UI's thread.
#include "X/Common/Shell/CmdLine.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef D3_MOD_LOAD_WORLD
#define D3_MOD_LOAD_WORLD ""
#endif
#ifndef D3_MOD_BLIZZCON
#define D3_MOD_BLIZZCON 0
#endif
#ifndef D3_MOD_DEV_TOOLS
#define D3_MOD_DEV_TOOLS 0
#endif

void D3ModDevStartup(CmdLineParams & tParams)
{
    const char * szWorld = getenv("D3_LOAD_WORLD");
    if (!szWorld)
    {
        szWorld = D3_MOD_LOAD_WORLD;
    }
    if (*szWorld)
    {
        snprintf(tParams.sLoadWorld, sizeof(tParams.sLoadWorld), "%s", szWorld);
        fprintf(stderr, "dev: starting in world %s\n", szWorld);
    }
    if (D3_MOD_BLIZZCON)
    {
        tParams.tBitArray.Set(CMDLINE_BLIZZCON_BIT, TRUE);
        fprintf(stderr, "dev: BlizzCon demo\n");
    }
}

#if D3_MOD_DEV_TOOLS
#include "X/Client/CGame.h"
#include "X/Client/CPlayer.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/GlobalSNO.h"
#include "X/Client/Camera/Camera.h"
#include "X/Client/Camera/CameraManager.h"

#include <math.h>
#include <time.h>

void CinematographyCutToStaticView(const SNO snoObserver, const Vector3D & wpCenter);  // X/Client/Camera/Cinematography.cpp
void CinematographyFixedView(RActorID idTargetRActor);                                 // X/Client/Camera/Cinematography.cpp
void CinematographyResetCameras();                                                     // X/Client/Camera/Cinematography.cpp
void CinematographyStaticView(const CameraState & cms);                                // X/Client/Camera/Cinematography.cpp
void CinematographyCutTo(const PRTransform & transform);                               // X/Client/Camera/Cinematography.cpp
namespace Console {
void ToggleTargetName();            // X/Client/Debug/TargetingDebugView.cpp
void ToggleControllerDebugView();   // X/Client/Debug/ControllerDebugView.cpp
}
namespace UIConsoleDesignerMenu {
void D3ModToggle(int32 nControllerIndex);  // src/mods/designer_menu.inc
}

extern "C" {
// src/host/window.h
enum { D3_HOTKEY_CAMERA = 1 << 3, D3_HOTKEY_TARGET_NAME = 1 << 4, D3_HOTKEY_STICK_VIEW = 1 << 5, D3_HOTKEY_DESIGNER = 1 << 6,
       D3_HOTKEY_FREECAM = 1 << 7 };
int d3_hotkeys_take_mask(int mask);
struct d3_pad_state {
    unsigned long long buttons;
    int leftX, leftY, rightX, rightY;
    int connected;
};
enum { D3_PAD_A = 0, D3_PAD_L = 6, D3_PAD_R = 7, D3_PAD_ZL = 8, D3_PAD_ZR = 9 };
void d3_input_capture(int on);
void d3_input_read_raw(struct d3_pad_state * out);
}

namespace {

bool sbFixedCamera;  // F5's view is on

struct FreeCamera {
    bool bOn;
    Vector3D wp;
    float flYaw, flPitch;  // radians; yaw from the world's X axis toward Y, pitch up from level
    double flLastTime;
};
FreeCamera stFree;

double sNow()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// A stick axis as -1..1, with a dead zone.
float sAxis(int nValue)
{
    float fl = nValue / 32767.0f;
    if (fabsf(fl) < 0.15f)
    {
        return 0.0f;
    }
    return fl > 1.0f ? 1.0f : fl < -1.0f ? -1.0f : fl;
}

Vector3D sLook(const FreeCamera & c)
{
    return Vector3D(cosf(c.flPitch) * cosf(c.flYaw), cosf(c.flPitch) * sinf(c.flYaw), sinf(c.flPitch));
}

void sFreeCameraApply()
{
    PRTransform t;
    t.q.FromViewVector(sLook(stFree), cgwvZAxis, cgwvInitCameraForward, cgwvInitCameraRight, cgwvInitCameraUp);
    t.wp = stFree.wp;
    CinematographyCutTo(t);
}

void sFreeCameraStart()
{
    CameraState cms = CameraManagerGetCameraState(CAMERA_CURRENT);
    Vector3D wvLook = cms.transform.q.Rotate(cgwvInitCameraForward);
    stFree.wp = cms.transform.wp;
    stFree.flYaw = atan2f(wvLook.y, wvLook.x);
    stFree.flPitch = asinf(wvLook.z < -1.0f ? -1.0f : wvLook.z > 1.0f ? 1.0f : wvLook.z);
    stFree.flLastTime = sNow();
    stFree.bOn = true;
    sbFixedCamera = false;
    CinematographyStaticView(cms);
    d3_input_capture(1);
    sFreeCameraApply();
}

void sFreeCameraStop()
{
    stFree.bOn = false;
    d3_input_capture(0);
    CinematographyFixedView(LocalPlayerRActorGet());
    CinematographyResetCameras();
}

void sFreeCameraUpdate()
{
    double flNow = sNow();
    float flDt = (float)(flNow - stFree.flLastTime);
    stFree.flLastTime = flNow;
    if (flDt > 0.1f)
    {
        flDt = 0.1f;  // after a hitch, don't jump
    }

    d3_pad_state pad;
    d3_input_read_raw(&pad);
    const float kTurn = 1.8f;    // radians a second at full stick
    const float kSpeed = 30.0f;  // world units a second at full stick (the hero walks about 20)
    float flSpeed = kSpeed * ((pad.buttons >> D3_PAD_A) & 1 ? 4.0f : 1.0f);

    stFree.flYaw -= sAxis(pad.rightX) * kTurn * flDt;
    stFree.flPitch += sAxis(pad.rightY) * kTurn * flDt;
    stFree.flPitch = stFree.flPitch > 1.5f ? 1.5f : stFree.flPitch < -1.5f ? -1.5f : stFree.flPitch;

    Vector3D wvLook = sLook(stFree);
    Vector3D wvRight(sinf(stFree.flYaw), -cosf(stFree.flYaw), 0.0f);
    float flForward = sAxis(pad.leftY) * flSpeed * flDt;
    float flSide = sAxis(pad.leftX) * flSpeed * flDt;
    float flUp = 0.0f;
    if ((pad.buttons >> D3_PAD_R) & 1 || (pad.buttons >> D3_PAD_ZR) & 1)
    {
        flUp += flSpeed * flDt;
    }
    if ((pad.buttons >> D3_PAD_L) & 1 || (pad.buttons >> D3_PAD_ZL) & 1)
    {
        flUp -= flSpeed * flDt;
    }
    stFree.wp.x += wvLook.x * flForward + wvRight.x * flSide;
    stFree.wp.y += wvLook.y * flForward + wvRight.y * flSide;
    stFree.wp.z += wvLook.z * flForward + flUp;
    sFreeCameraApply();
}

}  // namespace

void D3ModDevToolsUpdate()
{
    // In a game only; a key pressed before then waits.
    ActorCommonData * ptACD = CGameIsValid() ? ACDTryToGet(LocalPlayerACDGet()) : NULL;
    if (!ptACD)
    {
        if (stFree.bOn)
        {
            stFree.bOn = false;  // the game ended: give the controller back
            d3_input_capture(0);
        }
        return;
    }
    int keys = d3_hotkeys_take_mask(D3_HOTKEY_CAMERA | D3_HOTKEY_TARGET_NAME | D3_HOTKEY_STICK_VIEW | D3_HOTKEY_DESIGNER |
                                    D3_HOTKEY_FREECAM);
    if (keys & D3_HOTKEY_FREECAM)
    {
        if (!stFree.bOn)
        {
            sFreeCameraStart();
        }
        else
        {
            sFreeCameraStop();
        }
        fprintf(stderr, "dev: free camera %s\n", stFree.bOn ? "on" : "off");
    }
    else if (stFree.bOn)
    {
        sFreeCameraUpdate();
    }
    if (keys & D3_HOTKEY_CAMERA)
    {
        // As ClientMessageReceive.cpp's sSetScreenshotView: front (1), then back to the game (3).
        bool & bFixed = sbFixedCamera;
        if (stFree.bOn)
        {
            sFreeCameraStop();  // the screenshot camera replaces the free one
            bFixed = false;
        }
        if (!bFixed)
        {
            CinematographyCutToStaticView(GlobalSNOGet(GLOBALSNO_OBSERVER_SCREENSHOT_FRONT), ptACD->GetWorldLocation().transform.wp);
        }
        else
        {
            CinematographyFixedView(LocalPlayerRActorGet());
            CinematographyResetCameras();
        }
        bFixed = !bFixed;
        fprintf(stderr, "dev: screenshot camera %s\n", bFixed ? "on" : "off");
    }
    if (keys & D3_HOTKEY_TARGET_NAME)
    {
        Console::ToggleTargetName();
        fprintf(stderr, "dev: target name toggled\n");
    }
    if (keys & D3_HOTKEY_STICK_VIEW)
    {
        Console::ToggleControllerDebugView();
        fprintf(stderr, "dev: controller view toggled\n");
    }
    if (keys & D3_HOTKEY_DESIGNER)
    {
        UIConsoleDesignerMenu::D3ModToggle(0);
    }
}
#endif
