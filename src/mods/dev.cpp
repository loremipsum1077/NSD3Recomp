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

void CinematographyCutToStaticView(const SNO snoObserver, const Vector3D & wpCenter);  // X/Client/Camera/Cinematography.cpp
void CinematographyFixedView(RActorID idTargetRActor);                                 // X/Client/Camera/Cinematography.cpp
void CinematographyResetCameras();                                                     // X/Client/Camera/Cinematography.cpp
namespace Console {
void ToggleTargetName();            // X/Client/Debug/TargetingDebugView.cpp
void ToggleControllerDebugView();   // X/Client/Debug/ControllerDebugView.cpp
}
namespace UIConsoleDesignerMenu {
void D3ModToggle(int32 nControllerIndex);  // src/mods/designer_menu.inc
}

extern "C" {
// src/host/window.h
enum { D3_HOTKEY_CAMERA = 1 << 3, D3_HOTKEY_TARGET_NAME = 1 << 4, D3_HOTKEY_STICK_VIEW = 1 << 5, D3_HOTKEY_DESIGNER = 1 << 6 };
int d3_hotkeys_take_mask(int mask);
}

void D3ModDevToolsUpdate()
{
    // In a game only; a key pressed before then waits.
    ActorCommonData * ptACD = CGameIsValid() ? ACDTryToGet(LocalPlayerACDGet()) : NULL;
    if (!ptACD)
    {
        return;
    }
    int keys = d3_hotkeys_take_mask(D3_HOTKEY_CAMERA | D3_HOTKEY_TARGET_NAME | D3_HOTKEY_STICK_VIEW | D3_HOTKEY_DESIGNER);
    if (keys & D3_HOTKEY_CAMERA)
    {
        // As ClientMessageReceive.cpp's sSetScreenshotView: front (1), then back to the game (3).
        static bool bFixed = false;
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
