// Events offline (D3_MOD_EVENTS). On the Switch, Blizzard's lobby switched these on by
// setting the game's "Perk" and "AnniversaryEvent" cvars; offline they keep their
// defaults, so CMake changes the defaults instead (cmake/DecompSources.cmake):
//
//   Anniversary  AnniversaryEvent.Status = 1 (event running) opens the Darkening of
//                Tristram, and Perk.AnniversaryEnabled gives the anniversary buff. Without
//                the mod the dungeon still opens each January, from the game's own dates.
//   CowLevel     Perk.EasterEggWorldEnabled, which the server turns into the game flag
//                TotallyNotACowLevel.
//
// The double blood shards and double rift keystones events are community buffs, which the
// season theme mod (theme.cpp) turns on. This file only reports, once a hero is in a game,
// what the game made of the settings.
#include "X/Common/ActorCommonData.h"
#include "X/Common/Player.h"
#include "X/Server/LuaScript/ScriptUtil.h"
#include "X/Sigma/DataStructures/XVar.h"

#include <stdio.h>

extern XVarInt32 s_varAnniversaryEventStatus;     // X/Common/OnlineService/OnlineService.cpp
extern XVarBool s_varAnniversaryPerkEnabled;      // X/Common/CommunityBuffs.cpp
extern XVarBool s_varEasterEggWorldPerkEnabled;

void D3ModEventsUpdate()
{
    static bool bLogged = false;
    if (bLogged || PlayerGetFirstInGame() == NULL)
    {
        return;
    }
    bLogged = true;
    fprintf(stderr, "events: anniversary status %d (dungeon flag %d), anniversary buff %d, cow level %d (flag %d)\n",
            (int)s_varAnniversaryEventStatus.Get(), (int)GameFlagIsSet("RetroAnniversaryEvent_Dungeon"),
            (int)s_varAnniversaryPerkEnabled.Get(), (int)s_varEasterEggWorldPerkEnabled.Get(),
            (int)GameFlagIsSet("TotallyNotACowLevel"));
}
