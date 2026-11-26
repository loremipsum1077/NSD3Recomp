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
// "CommunityBuff"), e.g. "ShadowClones,FourthKanaisCubeSlot". The name "Season" stands for
// the theme of season D3_MOD_THEME_SEASON (the season mod's number), from the table below.
// The buffs apply to every hero, not only seasonal ones. In 2.7.7, Dark Alchemy (the Altar of Rites) and Nesting
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
#ifndef D3_MOD_THEME_SEASON
#define D3_MOD_THEME_SEASON 0
#endif

namespace {

// Each themed season's buffs, as Blizzard's servers set them. Season 23's theme (followers
// that fight like heroes) is built into 2.7.7 and has no buff. The list follows d3hack's
// season map (github.com/god-jester/d3hack, source/program/config.cpp).
const char * sSeasonTheme(int nSeason)
{
    switch (nSeason)
    {
    case 14: return "DoubleGoblins";                       // Season of Greed
    case 15: return "DoubleBountyBags";                    // Boon of the Horadrim
    case 16: return "RoyalGrandeur";                       // Season of Grandeur
    case 17: return "LegacyOfNightmares";                  // Season of Nightmares
    case 18: return "TriunesWill";                         // Season of the Triune
    case 19: return "Pandemonium";                         // Season of Eternal Conflict
    case 20: return "KanaiPowers";                         // Season of the Forbidden Archives
    case 21: return "TrialsOfTempests";                    // Trials of the Tempests
    case 22: return "ShadowClones,FourthKanaisCubeSlot";   // Shades of the Nephalem
    case 24: return "EtherealItems";                       // Ethereal Memory
    case 25: return "SoulShards";                          // The Lords of Hell
    case 26: return "SwarmRifts";                          // Echoing Nightmare
    case 27: return "SanctifiedItems";                     // Light's Calling
    case 28: return "DarkAlchemy";                         // Rites of Sanctuary
    case 29: return "NestingPortals";                      // Visions of Enmity
    case 30: return "SoulShards";
    case 31: return "KanaiPowers";
    case 32: return "EtherealItems";
    case 33: return "ShadowClones,FourthKanaisCubeSlot";
    case 34: return "SanctifiedItems";
    case 35: return "Pandemonium";
    case 36: return "SoulShards";
    case 37: return "KanaiPowers";
    default: return "";
    }
}

}  // namespace

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
        if (sName == "Season")
        {
            // The season's buffs go round the loop as if they had been listed.
            const char * szTheme = sSeasonTheme(D3_MOD_THEME_SEASON);
            fprintf(stderr, "theme: season %d's theme is %s\n", D3_MOD_THEME_SEASON, *szTheme ? szTheme : "none");
            sKeys += std::string(",") + szTheme;
        }
        else if (!sName.empty())
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
    fprintf(stderr, "theme: community buffs on: %s (event %s)\n", sKeys.c_str(),
            tBuffs.AnyActive() ? "active" : "inactive");
}
