// Season themes offline (D3_MOD_THEME). On the Switch, 2.7.7's season themes are
// "community buffs": Blizzard's lobby served a Config.txt with keys such as
// CommunityBuffSoulShards "1" and an event window, and the game (X/Common/CommunityBuffs.cpp)
// read them into its buff table, stored them in the Perk cvars, and the server put the
// buffs on each player from those cvars. Offline the lobby is never reached, so this does
// the same steps itself: once the server is running, it reads the chosen keys into the
// lobby's own table and stores them, as LobbyService::Idle does when an event window opens.
// The game puts the buffs on a hero when it joins; the hero already in the game when this
// runs is given them here.
//
// D3_MOD_THEME_KEYS is the comma-separated list of buff names (the part after
// "CommunityBuff"), e.g. "ShadowClones,FourthKanaisCubeSlot". The buffs apply to every
// hero, not only seasonal ones. In 2.7.7, Dark Alchemy (the Altar of Rites) and Nesting
// Portals are always on while any community event runs, so they come with every theme.
#include "X/Common/CommunityBuffs.h"
#include "X/Client/Definitions/PreferencesDefinition.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/Player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#ifndef D3_MOD_THEME_KEYS
#define D3_MOD_THEME_KEYS ""
#endif

void SPlayerApplyPerkBuffs(ActorCommonData & tACDPlayer);  // X/Server/SPlayer.cpp

namespace Console { namespace Online {
extern CommunityBuffs stCommunityBuffs;  // X/Client/Console/LobbyService.cpp
} }

void D3ModThemeUpdate()
{
    static bool bDone = false, bLogged = false;
    if (bDone)
    {
        // Once, for the first hero in a game: what the theme gave it.
        Player * ptPlayer = bLogged ? NULL : PlayerGetFirstInGame();
        ActorCommonData * ptACD = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
        if (ptACD)
        {
            bLogged = true;
            fprintf(stderr, "theme: hero: fourth cube slot %d, soul shards %d, ethereal %d, sanctified %d, dark alchemy %d\n",
                    (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_FOURTH_KANAIS_CUBE_SLOT)),
                    (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_SOUL_SHARDS_UNLOCKED)),
                    (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_ETHEREAL_ITEMS_UNLOCKED)),
                    (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_SANCTIFIED_ITEMS_UNLOCKED)),
                    (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_DARK_ALCHEMY_POWERS_UNLOCKED)));
        }
        return;
    }
    bDone = true;

    // The text Config.txt would hold: one 'key "value"' line per buff.
    std::string sConfig;
    std::string sKeys = D3_MOD_THEME_KEYS;
    size_t nAt = 0;
    while (nAt < sKeys.size())
    {
        size_t nEnd = sKeys.find(',', nAt);
        std::string sName = sKeys.substr(nAt, nEnd == std::string::npos ? std::string::npos : nEnd - nAt);
        nAt = nEnd == std::string::npos ? sKeys.size() : nEnd + 1;
        if (!sName.empty())
        {
            sConfig += "CommunityBuff" + sName + " \"1\"\n";
        }
    }

    KeyValueMap mapKeyValues;
    std::string sData = sConfig;  // the parser works in place
    Prefs::ParseFileDataToKeyValues(&sData[0], (uint32)sData.size(), mapKeyValues);

    CommunityBuffs & tBuffs = Console::Online::stCommunityBuffs;
    tBuffs.ResetAll();
    tBuffs.ReadConfig(mapKeyValues);
    s_varCommunityEventPerkEnabled.Set(tBuffs.AnyActive() != FALSE, XVarBase::DIRTY_DEFAULT);
    s_varCommunityEventPerkSeasonOnly.Set(false, XVarBase::DIRTY_DEFAULT);
    tBuffs.StoreToCvars();

    for (Player * ptPlayer = PlayerGetFirstInGame(); ptPlayer != NULL; ptPlayer = PlayerGetNextInGame(ptPlayer))
    {
        if (ActorCommonData * ptACD = ACDTryToGet(ptPlayer->idPlayerACD))
        {
            SPlayerApplyPerkBuffs(*ptACD);
        }
    }
    fprintf(stderr, "theme: community buffs on: %s (event %s)\n", D3_MOD_THEME_KEYS,
            tBuffs.AnyActive() ? "active" : "inactive");
}
