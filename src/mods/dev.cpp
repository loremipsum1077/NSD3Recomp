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
// The decomp's MainNX64.cpp is patched to call D3ModDevStartup() once, after the command line
// is parsed.
#include "X/Common/Shell/CmdLine.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef D3_MOD_LOAD_WORLD
#define D3_MOD_LOAD_WORLD ""
#endif
#ifndef D3_MOD_BLIZZCON
#define D3_MOD_BLIZZCON 0
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
