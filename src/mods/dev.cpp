// Developer starts: ways Blizzard's own builds could start the game that the Switch release
// still has the code for.
//
// D3_MOD_LOAD_WORLD=<world>  skip the menus and start a local game in that world, named by its
//     SNO name. The decomp's MainNX64.cpp still has the loader: when the name is set it makes
//     a local developer account with a new hero of each class and puts the party in that world
//     (Console::Online::CreateDummySetup). An unknown name falls back to the usual menus.
//     Worlds that ship and are not reached in normal play: "X1_TEST_ChallengeTestWorld" and
//     "Axe FlatWorld". Any other world name works too, such as "PvP_Duel_Small", the PvP
//     dueling world. D3_LOAD_WORLD, when starting the game, overrides it (empty for the menus).
//     D3_SCENE_SWAP=<old>:<new> swaps a scene for another (see D3ModSceneSwap below).
//     D3_SUBSCENE=<n> picks sub-scene <n> in every generated cluster (D3ModSubSceneOverride).
//     D3_PLAYERS=<1-4> starts that many local heroes (the -players option of Blizzard's PC
//     builds), one per controller. D3_GAME_TYPE=<type> starts the game as one of the game
//     types the release still has (D3ModDevGameType): tdm, the PvP team deathmatch.
//     D3_CLASS=<class> makes every developer hero that class (-class): dh, barb, wiz, wd,
//     monk, crusader or necro. They start with nothing equipped, so a Monk, whose attacks
//     need no weapon, can fight at once.
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
//         It is also a photo mode: X hides the HUD (the game's AppRenderLayerHide), Plus
//         pauses (GamePause), d-pad up and down change the field of view, left and right
//         toggle grayscale, B held moves slowly, Y takes a photo and Minus a top-down map
//         (see "Photo mode" below).
//
// The decomp's MainNX64.cpp is patched to call D3ModDevStartup() once, after the command line
// is parsed, and with the tools on, InputNX64.cpp to call D3ModDevToolsUpdate() once per input
// update, on the UI's thread.
#include "X/Common/Shell/CmdLine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char * szPlayers = getenv("D3_PLAYERS");
    if (szPlayers && *szPlayers)
    {
        int nPlayers = atoi(szPlayers);
        tParams.nTestPlayerNum = nPlayers < 1 ? 1 : nPlayers > 4 ? 4 : nPlayers;
        fprintf(stderr, "dev: %d local player%s\n", tParams.nTestPlayerNum, tParams.nTestPlayerNum == 1 ? "" : "s");
    }
    const char * szClass = getenv("D3_CLASS");
    if (szClass && *szClass)
    {
        static const char * const kClasses[] = {"dh", "barb", "wiz", "wd", "monk", "crusader", "necro"};
        for (int i = 0; i < 7; i++)
        {
            if (strcmp(szClass, kClasses[i]) == 0)
            {
                tParams.eTestClassOverride = (PlayerClass)i;
                fprintf(stderr, "dev: every hero a %s\n", kClasses[i]);
            }
        }
    }
    if (D3_MOD_BLIZZCON)
    {
        tParams.tBitArray.Set(CMDLINE_BLIZZCON_BIT, TRUE);
        fprintf(stderr, "dev: BlizzCon demo\n");
    }
}

// D3_GAME_TYPE=<type>: the game type a D3_LOAD_WORLD start is made with, instead of the
// release's -1 (none). The game templates (GameTemplate.cpp)
// still hold the PvP types 1.0 shipped and 2.7.7 never offers: tdm (4) is the team
// deathmatch, two teams with a pregame countdown, a timed round and a score to 300, run by
// PVPUpdate (X/Server/PVP.cpp). The other PvP numbers (2 dota, 3 arena, 5 ffa) may be given,
// untested. Game.cpp's GamePlayWorld, which starts a D3_LOAD_WORLD game, is patched to ask
// this.
int D3ModDevGameType()
{
    const char * szType = getenv("D3_GAME_TYPE");
    if (!szType || !*szType)
    {
        return -1;
    }
    static const struct { const char * szName; int nType; } kTypes[] = {
        {"dota", 2}, {"arena", 3}, {"tdm", 4}, {"ffa", 5},
    };
    int nType = -1;
    for (const auto & t : kTypes)
    {
        if (strcmp(szType, t.szName) == 0)
        {
            nType = t.nType;
        }
    }
    if (nType < 0 && szType[0] >= '0' && szType[0] <= '9')
    {
        nType = atoi(szType);
    }
    if (nType < 2 || nType > 5)
    {
        fprintf(stderr, "dev: game type %s is not one of dota, arena, tdm, ffa (2-5); ignored\n", szType);
        return -1;
    }
    fprintf(stderr, "dev: game type %d\n", nType);
    return nType;
}

#include "X/Common/Definitions/SceneDefinition.h"

static void D3ModSceneDescribe(unsigned int snoScene)
{
    ScopedSceneDefinitionLockR pdef((SNO)snoScene);
    if (!pdef)
    {
        fprintf(stderr, "dev:   scene %u: not loaded\n", snoScene);
        return;
    }
    const AABB & b = pdef->aabbBounds;
    fprintf(stderr, "dev:   scene %u: appearance %d, centre %.0f,%.0f,%.0f, half-size %.0f,%.0f,%.0f, flags 0x%x\n",
            snoScene, pdef->snoAppearance, b.wp.x, b.wp.y, b.wp.z, b.wvExt.x, b.wvExt.y, b.wvExt.z,
            (unsigned)pdef->dwFlags);
}

#include "X/Common/SNO/SNOFiles.h"

void SNOFileAddEntry(SNOTOC::TOCType eType, const SNOGroup snoGroup, const uint32 uGuid, LPCSTR szFileNameNoExtension);  // X/Common/SNO/SNOFiles.cpp

// Two test scenes ship in Common.cpk with their files, but CoreTOC.dat, the list the game finds
// files by number in, leaves them (and the one scene's appearance and collision mesh) out, so
// a lookup gets a default, empty scene. They are added to the list once the game has read it
// (SNOTOC::ReadIntoFileTable). Group numbers are CoreTOC.dat's: 33 scenes, 9 appearances,
// 61 collision meshes.
void D3ModSceneRegister()
{
    static const struct { SNOGroup snoGroup; uint32 uGuid; const char * szName; } kFiles[] = {
        {33, 345754, "x1_westm_Monster_Affix_Test"},
        {33, 121112, "Test_EventLeavingLevelArea"},
        {9, 121115, "Test_EventLeavingLevelArea"},
        {61, 128638, "Test_EventLeavingLevelArea"},
    };
    for (const auto & tFile : kFiles)
    {
        SNOFileAddEntry(SNOTOC::TT_CORE, tFile.snoGroup, tFile.uGuid, tFile.szName);
    }
}

// D3_SUBSCENE=<n>: the -subscenes option of Blizzard's PC builds, which the Switch release
// still honours but never sets. A random dungeon is built from scene clusters, each of which
// picks one of its sub-scene variants (the rooms and set pieces placed in it) at random;
// with this set, each cluster takes its variant <n> instead, counting through its weighted
// groups and then its default group. AppGlobals.nSubSceneOverride is set from this (Game.cpp),
// and DRLG.cpp is patched to use it only for clusters that have a variant <n>: the release
// leaves a cluster without one empty.
int D3ModSubSceneOverride()
{
    const char * szValue = getenv("D3_SUBSCENE");
    if (!szValue || !*szValue)
    {
        return -1;
    }
    int nIndex = atoi(szValue);
    fprintf(stderr, "dev: sub-scene %d in every cluster that has one\n", nIndex);
    return nIndex < 0 ? -1 : nIndex;
}

// D3_SCENE_SWAP=<old>:<new> (scene SNO numbers; <old> may be *) puts scene <new> wherever a
// world places <old>, so a scene no world uses can be seen in a test world. The server's
// SSceneAdd (X/Server/SScene.cpp) asks this for every scene it adds.
unsigned int D3ModSceneSwap(unsigned int snoScene)
{
    static int snState;  // 0 unread, 1 off, 2 on
    static unsigned int snoOld, snoNew;
    static bool sbAny;
    if (snState == 0)
    {
        snState = 1;
        const char * sz = getenv("D3_SCENE_SWAP");
        const char * szColon = sz ? strchr(sz, ':') : NULL;
        if (szColon)
        {
            sbAny = sz[0] == '*';
            snoOld = (unsigned int)strtoul(sz, NULL, 10);
            snoNew = (unsigned int)strtoul(szColon + 1, NULL, 10);
            snState = 2;
        }
    }
    unsigned int snoResult = snoScene;
    if (snState == 2 && (sbAny || snoScene == snoOld))
    {
        snoResult = snoNew;
    }
    if (snoResult != snoScene)
    {
        fprintf(stderr, "dev: scene %u -> %u\n", snoScene, snoResult);
        D3ModSceneDescribe(snoScene);
        D3ModSceneDescribe(snoResult);
    }
    return snoResult;
}

#if D3_MOD_DEV_TOOLS
#include "X/Client/CGame.h"
#include "X/Common/Game.h"
#include "X/Client/CPlayer.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/GlobalSNO.h"
#include "X/Client/Camera/Camera.h"
#include "X/Client/Camera/CameraManager.h"

#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
enum { D3_PAD_A = 0, D3_PAD_B, D3_PAD_X, D3_PAD_Y, D3_PAD_L = 6, D3_PAD_R, D3_PAD_ZL, D3_PAD_ZR, D3_PAD_PLUS, D3_PAD_MINUS,
       D3_PAD_LEFT, D3_PAD_UP, D3_PAD_RIGHT, D3_PAD_DOWN };
void d3_input_capture(int on);
void d3_input_read_raw(struct d3_pad_state * out);
// src/gfx/metal/nvn_metal.mm
void d3_capture_next(const char * path);
unsigned d3_captures_done(void);
unsigned d3_frames_presented(void);
int d3_image_stitch(const char * out, const char * const * tiles, int cols, int rows, int outWidth, int outHeight, float gamma);
void d3_present_size(int * width, int * height);
}
void AppRenderLayerHide(int32 nFlag, BOOL bValue);  // X/Common/Game.cpp
void AppDrawFlagSet(int32 nFlag, BOOL bValue);      // X/Common/Game.cpp
BOOL AppDrawFlagIsSet(int32 nFlag);                 // X/Common/Game.cpp
BOOL AppRenderLayerIsHidden(int32 nFlag);           // X/Common/Game.cpp
enum { D3MOD_RENDERLAYER_UI = 10, D3MOD_RENDERLAYER_UI_OVERLAY = 19 };  // X/Client/Gfx/GfxCommon.h

namespace {

bool sbFixedCamera;  // F5's view is on

struct FreeCamera {
    bool bOn;
    Vector3D wp;
    float flYaw, flPitch;  // radians; yaw from the world's X axis toward Y, pitch up from level
    float flFOV;           // radians, vertical
    double flLastTime;
    unsigned long long uLastButtons;
    bool bHidUI, bPaused;  // photo mode's doing, undone when the free camera stops
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
    CameraState cms = CameraManagerGetCameraState(CAMERA_CURRENT);
    if (cms.aFOV != stFree.flFOV || !cms.bPerspective)
    {
        cms.bPerspective = TRUE;
        cms.aFOV = stFree.flFOV;
        cms.transform = t;
        CinematographyStaticView(cms);
    }
    CinematographyCutTo(t);
}

void sSetUIHidden(bool bHidden)
{
    AppRenderLayerHide(D3MOD_RENDERLAYER_UI, bHidden);
    AppRenderLayerHide(D3MOD_RENDERLAYER_UI_OVERLAY, bHidden);
}

// ---- Photo mode: captures of several presented frames stitched into one picture ----
//
// A photo is the free camera's view at three times the size (D3_PHOTO_TILES across and
// down): the game's own high-resolution screenshot setting (CameraManagerSetHighResScreenshotMode,
// left over from Blizzard's PC tools) narrows the projection to one tile at a time, and
// each tile is a separate frame. A map is the area around the camera seen straight down
// with no perspective, D3_MAP_SIZE world units across, one tile of D3_MAP_TILE units per
// frame. While either runs the game is paused and the HUD hidden. Pictures go to
// D3_PHOTO_DIR (default ~/Pictures/Diablo III).

enum CaptureKind { CAPTURE_NONE, CAPTURE_PHOTO, CAPTURE_MAP };
enum { D3MOD_APP_DRAW_NO_FOG_BIT = 17 };  // X/Common/Game.h

struct Capture {
    CaptureKind eKind;
    int nCols, nRows, nTile;
    int nWaitFrames;          // presented frames to let pass before the next capture
    unsigned uFrameMark;      // d3_frames_presented when the current tile was set
    unsigned uCaptureMark;    // d3_captures_done when the current tile was asked for
    bool bRequested;
    bool bWasHidden, bWasPaused, bHadNoFog;
    CameraState cmsBefore;    // the free camera's own, put back afterwards
    char szOut[1024];
    char aszTiles[256][1040];
    // map
    Vector3D wpCentre, wvRight, wvUp;
    float flTileW, flTileH, flCameraZ;
};
Capture stCapture;

int sEnvInt(const char * szName, int nDefault, int nMin, int nMax)
{
    const char * sz = getenv(szName);
    int n = sz && *sz ? atoi(sz) : nDefault;
    return n < nMin ? nMin : n > nMax ? nMax : n;
}

void sPhotoPath(char * szOut, size_t cb, const char * szKind)
{
    char szDir[900];
    const char * szEnv = getenv("D3_PHOTO_DIR");
    if (szEnv && *szEnv)
    {
        snprintf(szDir, sizeof szDir, "%s", szEnv);
    }
    else
    {
        const char * szHome = getenv("HOME");
        snprintf(szDir, sizeof szDir, "%s/Pictures", szHome ? szHome : ".");
        mkdir(szDir, 0755);
        snprintf(szDir, sizeof szDir, "%s/Pictures/Diablo III", szHome ? szHome : ".");
    }
    mkdir(szDir, 0755);
    time_t now = time(NULL);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char szStamp[32];
    strftime(szStamp, sizeof szStamp, "%Y-%m-%d %H.%M.%S", &tmNow);
    snprintf(szOut, cb, "%s/%s %s.png", szDir, szKind, szStamp);
}

void sCaptureSetTile()
{
    Capture & c = stCapture;
    if (c.eKind == CAPTURE_PHOTO)
    {
        // The game's tiles count rows from the bottom of the picture; ours from the top.
        int nCol = c.nTile % c.nCols, nRow = c.nTile / c.nCols;
        CameraManagerSetHighResScreenshotMode((uint32)((c.nRows - 1 - nRow) * c.nCols + nCol), (uint32)c.nCols);
        sFreeCameraApply();
    }
    else
    {
        int nCol = c.nTile % c.nCols, nRow = c.nTile / c.nCols;
        float flX = (nCol - (c.nCols - 1) * 0.5f) * c.flTileW;
        float flY = ((c.nRows - 1) * 0.5f - nRow) * c.flTileH;
        PRTransform t;
        t.q.FromViewVector(Vector3D(0.0f, 0.0f, -1.0f), c.wvUp, cgwvInitCameraForward, cgwvInitCameraRight, cgwvInitCameraUp);
        t.wp = Vector3D(c.wpCentre.x + c.wvRight.x * flX + c.wvUp.x * flY, c.wpCentre.y + c.wvRight.y * flX + c.wvUp.y * flY, c.flCameraZ);
        CameraState cms = CameraManagerGetCameraState(CAMERA_CURRENT);
        CameraStateSetOrthographic(cms, t, c.flTileH, cms.flAspectRatio, 1.0f, 2000.0f, cms.rectViewport, cms.vecScale, cms.vecProjectionTranslate);
        CinematographyStaticView(cms);
        CinematographyCutTo(t);
    }
    snprintf(c.aszTiles[c.nTile], sizeof c.aszTiles[c.nTile], "%s.tile%d.png", c.szOut, c.nTile);
    c.uFrameMark = d3_frames_presented();
    c.bRequested = false;
}

void sCaptureStart(CaptureKind eKind, const ActorCommonData & tHero)
{
    Capture & c = stCapture;
    c.eKind = eKind;
    c.nTile = 0;
    c.nWaitFrames = sEnvInt("D3_PHOTO_WAIT", 4, 2, 120);
    c.bWasHidden = AppRenderLayerIsHidden(D3MOD_RENDERLAYER_UI) != FALSE;
    c.bWasPaused = GameIsPaused() != FALSE;
    c.cmsBefore = CameraManagerGetCameraState(CAMERA_CURRENT);
    if (eKind == CAPTURE_PHOTO)
    {
        c.nCols = c.nRows = sEnvInt("D3_PHOTO_TILES", 3, 1, 8);
        sPhotoPath(c.szOut, sizeof c.szOut, "Photo");
    }
    else
    {
        CameraState cms = CameraManagerGetCameraState(CAMERA_CURRENT);
        float flSize = (float)sEnvInt("D3_MAP_SIZE", 360, 20, 4000);
        c.flTileH = (float)sEnvInt("D3_MAP_TILE", 60, 5, 1000);
        c.flTileW = c.flTileH * (cms.flAspectRatio > 0.1f ? cms.flAspectRatio : 16.0f / 9.0f);
        c.nCols = (int)ceilf(flSize / c.flTileW);
        c.nRows = (int)ceilf(flSize / c.flTileH);
        while (c.nCols * c.nRows > 256)
        {
            c.flTileH *= 1.25f;
            c.flTileW *= 1.25f;
            c.nCols = (int)ceilf(flSize / c.flTileW);
            c.nRows = (int)ceilf(flSize / c.flTileH);
        }
        // Square to the world (whose scenes are laid out on its axes), with the world axis
        // nearest the view's forward at the top.
        float flYaw = roundf(stFree.flYaw / (float)(M_PI / 2)) * (float)(M_PI / 2);
        // Centred where the view's middle meets the ground at the hero's height.
        float flGroundZ = tHero.GetWorldLocation().transform.wp.z;
        Vector3D wvLook = sLook(stFree);
        c.wpCentre = stFree.wp;
        if (wvLook.z < -0.2f && stFree.wp.z > flGroundZ)
        {
            float flT = (stFree.wp.z - flGroundZ) / -wvLook.z;
            c.wpCentre = Vector3D(stFree.wp.x + wvLook.x * flT, stFree.wp.y + wvLook.y * flT, flGroundZ);
        }
        c.wvUp = Vector3D(roundf(cosf(flYaw)), roundf(sinf(flYaw)), 0.0f);
        c.wvRight = Vector3D(c.wvUp.y, -c.wvUp.x, 0.0f);
        c.flCameraZ = tHero.GetWorldLocation().transform.wp.z + (float)sEnvInt("D3_MAP_HEIGHT", 120, 10, 2000);
        sPhotoPath(c.szOut, sizeof c.szOut, "Map");
    }
    sSetUIHidden(true);
    c.bHadNoFog = AppDrawFlagIsSet(D3MOD_APP_DRAW_NO_FOG_BIT) != FALSE;
    if (eKind == CAPTURE_MAP)
    {
        AppDrawFlagSet(D3MOD_APP_DRAW_NO_FOG_BIT, TRUE);  // straight down from high up, fog would hide everything
    }
    if (!c.bWasPaused)
    {
        GamePause(TRUE);
    }
    fprintf(stderr, "dev: %s, %dx%d tiles, to %s\n", eKind == CAPTURE_PHOTO ? "photo" : "map", c.nCols, c.nRows, c.szOut);
    sCaptureSetTile();
}

void sCaptureFinish()
{
    Capture & c = stCapture;
    if (c.eKind == CAPTURE_PHOTO)
    {
        CameraManagerSetHighResScreenshotMode(0, 0);
    }
    int nWidth = 0, nHeight = 0;
    d3_present_size(&nWidth, &nHeight);
    if (nWidth <= 0 || nHeight <= 0)
    {
        nWidth = 1600;
        nHeight = 900;
    }
    long nOutW = (long)nWidth * c.nCols, nOutH = (long)nHeight * c.nRows;
    long nMax = sEnvInt("D3_PHOTO_MAX", 16384, 1024, 32768);
    if (nOutW > nMax || nOutH > nMax)
    {
        double fl = (double)nMax / (nOutW > nOutH ? nOutW : nOutH);
        nOutW = (long)(nOutW * fl);
        nOutH = (long)(nOutH * fl);
    }
    const char * apszTiles[256];
    for (int i = 0; i < c.nCols * c.nRows; i++)
    {
        apszTiles[i] = c.aszTiles[i];
    }
    // Maps are lit for play, by the hero's light and little else; D3_MAP_GAMMA (percent,
    // default 55) brightens the dark parts so the layout can be read.
    float flGamma = c.eKind == CAPTURE_MAP ? sEnvInt("D3_MAP_GAMMA", 55, 10, 300) / 100.0f : 1.0f;
    d3_image_stitch(c.szOut, apszTiles, c.nCols, c.nRows, (int)nOutW, (int)nOutH, flGamma);
    if (!getenv("D3_PHOTO_KEEP_TILES"))
    {
        for (int i = 0; i < c.nCols * c.nRows; i++)
        {
            unlink(c.aszTiles[i]);
        }
    }
    sSetUIHidden(c.bWasHidden || stFree.bHidUI);
    AppDrawFlagSet(D3MOD_APP_DRAW_NO_FOG_BIT, c.bHadNoFog);
    if (!c.bWasPaused && !stFree.bPaused)
    {
        GamePause(FALSE);
    }
    c.eKind = CAPTURE_NONE;
    CinematographyStaticView(c.cmsBefore);
    sFreeCameraApply();  // back to the free camera's own view
}

// The game's own debug render modes, as photo filters (X/Common/Game.h DebugRenderMode):
// none and grayscale. (DRM_RETRO, the Darkening of Tristram look, draws black here: its
// post effect doesn't yet work on the Metal renderer.)
void sNextFilter()
{
    static const DebugRenderMode keFilters[] = {DRM_DISABLED, DRM_GRAYSCALE};
    static const char * const kszNames[] = {"none", "grayscale"};
    const int kCount = 2;
    int n = 0;
    while (n < kCount && sgtAppGlobals.eDebugRenderMode != keFilters[n])
    {
        n++;
    }
    n = (n + 1) % kCount;
    sgtAppGlobals.eDebugRenderMode = keFilters[n];
    fprintf(stderr, "dev: filter %s\n", kszNames[n]);
}

// One step a frame: wait for the tile's frame to come round, ask for it, wait for it to be saved.
void sCaptureUpdate()
{
    Capture & c = stCapture;
    if (!c.bRequested)
    {
        if (d3_frames_presented() - c.uFrameMark >= (unsigned)c.nWaitFrames)
        {
            c.uCaptureMark = d3_captures_done();
            d3_capture_next(c.aszTiles[c.nTile]);
            c.bRequested = true;
        }
        return;
    }
    if (d3_captures_done() == c.uCaptureMark)
    {
        return;
    }
    if (++c.nTile < c.nCols * c.nRows)
    {
        sCaptureSetTile();
    }
    else
    {
        sCaptureFinish();
    }
}

void sFreeCameraStart()
{
    CameraState cms = CameraManagerGetCameraState(CAMERA_CURRENT);
    Vector3D wvLook = cms.transform.q.Rotate(cgwvInitCameraForward);
    stFree.wp = cms.transform.wp;
    stFree.flYaw = atan2f(wvLook.y, wvLook.x);
    stFree.flPitch = asinf(wvLook.z < -1.0f ? -1.0f : wvLook.z > 1.0f ? 1.0f : wvLook.z);
    stFree.flFOV = cms.bPerspective && cms.aFOV > 0.05f ? cms.aFOV : 0.8f;
    stFree.flLastTime = sNow();
    stFree.uLastButtons = ~0ull;  // buttons held when it starts do nothing until let go
    stFree.bHidUI = stFree.bPaused = false;
    stFree.bOn = true;
    sbFixedCamera = false;
    CinematographyStaticView(cms);
    d3_input_capture(1);
    sFreeCameraApply();
}

void sFreeCameraStop()
{
    if (stCapture.eKind != CAPTURE_NONE)
    {
        sCaptureFinish();
    }
    if (stFree.bHidUI)
    {
        sSetUIHidden(false);
    }
    if (stFree.bPaused)
    {
        GamePause(FALSE);
    }
    if (sgtAppGlobals.eDebugRenderMode == DRM_GRAYSCALE)
    {
        sgtAppGlobals.eDebugRenderMode = DRM_DISABLED;  // the filter is photo mode's
    }
    stFree.bHidUI = stFree.bPaused = false;
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
    unsigned long long uPressed = pad.buttons & ~stFree.uLastButtons;
    stFree.uLastButtons = pad.buttons;
    if (stCapture.eKind != CAPTURE_NONE)
    {
        sCaptureUpdate();  // the camera stays put until the picture is done
        return;
    }
    if ((uPressed >> D3_PAD_X) & 1)
    {
        stFree.bHidUI = !stFree.bHidUI;
        sSetUIHidden(stFree.bHidUI);
        fprintf(stderr, "dev: HUD %s\n", stFree.bHidUI ? "hidden" : "shown");
    }
    if ((uPressed >> D3_PAD_LEFT) & 1 || (uPressed >> D3_PAD_RIGHT) & 1)
    {
        sNextFilter();
    }
    if ((uPressed >> D3_PAD_PLUS) & 1)
    {
        stFree.bPaused = !stFree.bPaused;
        GamePause(stFree.bPaused);
        fprintf(stderr, "dev: game %s\n", stFree.bPaused ? "paused" : "running");
    }
    ActorCommonData * ptHero = ACDTryToGet(LocalPlayerACDGet());
    if (((uPressed >> D3_PAD_Y) & 1) && ptHero)
    {
        sCaptureStart(CAPTURE_PHOTO, *ptHero);
        return;
    }
    if (((uPressed >> D3_PAD_MINUS) & 1) && ptHero)
    {
        sCaptureStart(CAPTURE_MAP, *ptHero);
        return;
    }
    const float kTurn = 1.8f;    // radians a second at full stick
    const float kSpeed = 30.0f;  // world units a second at full stick (the hero walks about 20)
    float flSpeed = kSpeed * ((pad.buttons >> D3_PAD_A) & 1 ? 4.0f : (pad.buttons >> D3_PAD_B) & 1 ? 0.25f : 1.0f);
    float flZoom = ((pad.buttons >> D3_PAD_UP) & 1 ? -1.0f : 0.0f) + ((pad.buttons >> D3_PAD_DOWN) & 1 ? 1.0f : 0.0f);
    stFree.flFOV *= expf(flZoom * 0.8f * flDt);  // up narrows the view (zooms in), down widens it
    stFree.flFOV = stFree.flFOV < 0.1f ? 0.1f : stFree.flFOV > 2.2f ? 2.2f : stFree.flFOV;

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

bool D3ModPhotoPlainColor()  // asked by PostFX.cpp, on the render thread
{
    return stCapture.eKind != CAPTURE_NONE;
}

void D3ModDevToolsUpdate()
{
    // In a game only; a key pressed before then waits.
    ActorCommonData * ptACD = CGameIsValid() ? ACDTryToGet(LocalPlayerACDGet()) : NULL;
    if (!ptACD)
    {
        if (stFree.bOn)
        {
            stFree.bOn = false;  // the game ended: give the controller back
            stCapture.eKind = CAPTURE_NONE;
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
