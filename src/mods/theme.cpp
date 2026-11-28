// Season themes offline (D3_MOD_THEME). On the Switch, 2.7.7's season themes are
// "community buffs": Blizzard's lobby served a Config.txt with keys such as
// CommunityBuffSoulShards "1" and an event window, and the game (X/Common/CommunityBuffs.cpp)
// read them into its buff table, stored them in the Perk cvars, and the server put the
// buffs on each player from those cvars. Offline the lobby is never reached, so this does
// the same steps itself: it reads the chosen keys into the lobby's own table and stores
// them, as LobbyService::Idle does when an event window opens, and gives the buffs to the
// heroes already in the game (the game gives them to a hero when it joins).
//
// D3_MOD_THEME_KEYS is the comma-separated list of buff names (the part after
// "CommunityBuff"), e.g. "ShadowClones,FourthKanaisCubeSlot". The name "Season" stands for
// the theme of season D3_MOD_THEME_SEASON (the season mod's number), from the table below;
// "Pick" starts with no theme. The buffs apply to every hero, not only seasonal ones. In
// 2.7.7, Dark Alchemy (the Altar of Rites) and Nesting Portals are always on while any
// community event runs, so they come with every theme.
//
// The theme can be changed while the game runs. A request is a season number (14 to 37),
// "off", "default" (the build's theme) or buff names as above, and comes from:
//   <save>/theme/request.txt   written by the Mods page's Season theme tab (tools/mods.py)
//   D3ModThemeRequest()        the designer menu's Game page (src/mods/designer_menu.inc)
// A switch takes the old theme's powers off every hero and puts the new ones on, through
// the server's own SPlayerApplyPerkBuffs. The choice is kept in <save>/theme/theme.txt and
// comes back at the next start; "default" forgets it. The game lists the themes in
// <save>/theme/themes.json and answers in <save>/theme/status.json.
#include "X/Common/CommunityBuffs.h"
#include "X/Client/Definitions/PreferencesDefinition.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/GlobalSNO.h"
#include "X/Common/Player.h"
#include "X/Server/Powers/PowerAPI.h"
#include "X/Server/SGame.h"
#include "X/Server/SPlayer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mutex>
#include <string>

#ifndef D3_MOD_THEME_KEYS
#define D3_MOD_THEME_KEYS ""
#endif
#ifndef D3_MOD_THEME_EVENT_KEYS
#define D3_MOD_THEME_EVENT_KEYS ""  // the events mod's buffs (DoubleBloodShards...), on with every theme
#endif
#ifndef D3_MOD_THEME_SEASON
#define D3_MOD_THEME_SEASON 0
#endif

extern "C" const char * d3_save_dir(void);  // src/nn/fs/fs.cpp
void SPlayerApplyPerkBuffs(ActorCommonData & tACDPlayer);  // X/Server/SPlayer.cpp

namespace Console { namespace Online {
extern CommunityBuffs stCommunityBuffs;  // X/Client/Console/LobbyService.cpp
} }

namespace {

struct SeasonTheme
{
    int nSeason;
    const char * szName;
    const char * szKeys;
};

// Each themed season's buffs, as Blizzard's servers set them. Season 23's theme (followers
// that fight like heroes) is built into 2.7.7 and has no buff. The list follows d3hack's
// season map (github.com/god-jester/d3hack, source/program/config.cpp); seasons 30 on
// brought back earlier themes.
const SeasonTheme sarThemes[] = {
    {14, "Season of Greed", "DoubleGoblins"},
    {15, "Boon of the Horadrim", "DoubleBountyBags"},
    {16, "Season of Grandeur", "RoyalGrandeur"},
    {17, "Season of Nightmares", "LegacyOfNightmares"},
    {18, "Season of the Triune", "TriunesWill"},
    {19, "Season of Eternal Conflict", "Pandemonium"},
    {20, "Season of the Forbidden Archives", "KanaiPowers"},
    {21, "Trials of the Tempests", "TrialsOfTempests"},
    {22, "Shades of the Nephalem", "ShadowClones,FourthKanaisCubeSlot"},
    {24, "Ethereal Memory", "EtherealItems"},
    {25, "The Lords of Hell", "SoulShards"},
    {26, "Echoing Nightmare", "SwarmRifts"},
    {27, "Light's Calling", "SanctifiedItems"},
    {28, "Rites of Sanctuary", "DarkAlchemy"},
    {29, "Visions of Enmity", "NestingPortals"},
    {30, "The Lords of Hell", "SoulShards"},
    {31, "Season of the Forbidden Archives", "KanaiPowers"},
    {32, "Ethereal Memory", "EtherealItems"},
    {33, "Shades of the Nephalem", "ShadowClones,FourthKanaisCubeSlot"},
    {34, "Light's Calling", "SanctifiedItems"},
    {35, "Season of Eternal Conflict", "Pandemonium"},
    {36, "The Lords of Hell", "SoulShards"},
    {37, "Season of the Forbidden Archives", "KanaiPowers"},
};

const SeasonTheme * sFindSeason(int nSeason)
{
    for (const SeasonTheme & tTheme : sarThemes)
    {
        if (tTheme.nSeason == nSeason)
        {
            return &tTheme;
        }
    }
    return NULL;
}

// The powers a theme puts on a hero: the community event's own (which carries the buffs
// that work through attributes) and the nine 2.7.7 features with a power of their own.
const GlobalSNO sareThemePowers[] = {
    GLOBALSNO_POWERS_COMMUNITY_EVENT_XPMF,
    GLOBALSNO_UNK_1555, GLOBALSNO_UNK_1556, GLOBALSNO_UNK_1557, GLOBALSNO_UNK_1558, GLOBALSNO_UNK_1559,
    GLOBALSNO_UNK_1560, GLOBALSNO_UNK_1561, GLOBALSNO_UNK_1562, GLOBALSNO_UNK_1563,
};

std::mutex sgLock;
std::string sgsQueued;        // from D3ModThemeRequest, any thread
std::string sgsCurrent;       // the request in force, as asked for
std::string sgsCurrentKeys;   // and its buff names

std::string sFolder()
{
    std::string sFolder = std::string(d3_save_dir()) + "/theme";
    mkdir(d3_save_dir(), 0755);
    mkdir(sFolder.c_str(), 0755);
    return sFolder;
}

void sWriteFile(const std::string & sPath, const std::string & sText)
{
    std::string sTemporary = sPath + ".tmp";
    FILE * ptFile = fopen(sTemporary.c_str(), "wb");
    if (!ptFile)
    {
        return;
    }
    fwrite(sText.data(), 1, sText.size(), ptFile);
    fclose(ptFile);
    rename(sTemporary.c_str(), sPath.c_str());
}

std::string sReadLine(const std::string & sPath)
{
    char szLine[256] = "";
    if (FILE * ptFile = fopen(sPath.c_str(), "rb"))
    {
        if (!fgets(szLine, sizeof szLine, ptFile))
        {
            szLine[0] = 0;
        }
        fclose(ptFile);
    }
    szLine[strcspn(szLine, "\r\n")] = 0;
    return szLine;
}

std::string sTrim(const std::string & s)
{
    size_t nStart = s.find_first_not_of(" \t");
    size_t nEnd = s.find_last_not_of(" \t");
    return nStart == std::string::npos ? std::string() : s.substr(nStart, nEnd - nStart + 1);
}

// A request's buff names, or false when it is not one this understands.
bool sResolve(const std::string & sRequest, std::string & sKeys)
{
    std::string s = sTrim(sRequest);
    if (s == "default")
    {
        s = D3_MOD_THEME_KEYS;
    }
    if (s.empty() || s == "off" || s == "Off" || s == "Pick")
    {
        sKeys.clear();
        return true;
    }
    if (isdigit((unsigned char)s[0]))
    {
        const SeasonTheme * ptTheme = sFindSeason(atoi(s.c_str()));
        if (!ptTheme)
        {
            return false;
        }
        sKeys = ptTheme->szKeys;
        return true;
    }
    // Buff names; "Season" stands for the build's season's theme.
    sKeys.clear();
    size_t nAt = 0;
    while (nAt < s.size())
    {
        size_t nEnd = s.find(',', nAt);
        std::string sName = sTrim(s.substr(nAt, nEnd == std::string::npos ? std::string::npos : nEnd - nAt));
        nAt = nEnd == std::string::npos ? s.size() : nEnd + 1;
        if (sName == "Season")
        {
            const SeasonTheme * ptTheme = sFindSeason(D3_MOD_THEME_SEASON);
            sName = ptTheme ? ptTheme->szKeys : "";
        }
        else if (sName == "Pick")
        {
            sName.clear();
        }
        for (char c : sName)
        {
            if (!isalpha((unsigned char)c) && c != ',')
            {
                return false;
            }
        }
        if (!sName.empty())
        {
            sKeys += (sKeys.empty() ? "" : ",") + sName;
        }
    }
    return true;
}

// What the request is called on the page and in the log.
std::string sDescribe(const std::string & sRequest, const std::string & sKeys)
{
    std::string s = sTrim(sRequest);
    if (!s.empty() && isdigit((unsigned char)s[0]))
    {
        const SeasonTheme * ptTheme = sFindSeason(atoi(s.c_str()));
        return "Season " + s + ": " + (ptTheme ? ptTheme->szName : "?");
    }
    return sKeys.empty() ? std::string("off") : sKeys;
}

std::string sJson(const std::string & s)
{
    std::string sOut = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
        {
            sOut += '\\';
        }
        sOut += c;
    }
    return sOut + "\"";
}

void sWriteThemes()
{
    std::string sText = "[\n";
    for (size_t i = 0; i < sizeof(sarThemes) / sizeof(sarThemes[0]); i++)
    {
        sText += "  {\"season\": " + std::to_string(sarThemes[i].nSeason) + ", \"name\": " + sJson(sarThemes[i].szName) +
                 ", \"buffs\": " + sJson(sarThemes[i].szKeys) + (i + 1 < sizeof(sarThemes) / sizeof(sarThemes[0]) ? "},\n" : "}\n");
    }
    sWriteFile(sFolder() + "/themes.json", sText + "]\n");
}

void sWriteStatus(const std::string & sMessage)
{
    std::string sRequest = sTrim(sgsCurrent);
    if (sRequest == "Season" && D3_MOD_THEME_SEASON)
    {
        sRequest = std::to_string(D3_MOD_THEME_SEASON);  // the row the page marks
    }
    std::string sText = "{\n  \"updated\": " + std::to_string((long long)time(NULL)) +
                        ",\n  \"request\": " + sJson(sRequest) +
                        ",\n  \"season\": " + std::to_string(!sRequest.empty() && isdigit((unsigned char)sRequest[0]) ? atoi(sRequest.c_str()) : 0) +
                        ",\n  \"buffs\": " + sJson(sgsCurrentKeys) +
                        ",\n  \"default\": " + sJson(D3_MOD_THEME_KEYS) +
                        ",\n  \"message\": " + sJson(sMessage) + "\n}\n";
    sWriteFile(sFolder() + "/status.json", sText);
}

// Takes every theme power off a hero, and the two buffs that work through game flags.
void sStripTheme(Player & tPlayer, ActorCommonData & tACD)
{
    for (GlobalSNO eGlobalSNO : sareThemePowers)
    {
        SNO snoPower = GlobalSNOGet(eGlobalSNO);
        if (snoPower == SNO_NONE)
        {
            continue;
        }
        BuffID idBuff;
        while ((idBuff = PowerAPI_BuffFindOnACD(snoPower, tACD, ACDID_INVALID)) != BUFFID_INVALID)
        {
            PowerAPI_BuffFlagForDeletion(idBuff, FALSE, TRUE, FALSE, TRUE);
        }
    }
    PlayerSetPlayerGameFlag(&tPlayer, "DoubleRiftKeystones", FALSE);
    SGameSetCommunityBuffDoubleGoblins(FALSE);
}

void sApply(const std::string & sKeys)
{
    for (Player * ptPlayer = PlayerGetFirstInGame(); ptPlayer != NULL; ptPlayer = PlayerGetNextInGame(ptPlayer))
    {
        if (ActorCommonData * ptACD = ACDTryToGet(ptPlayer->idPlayerACD))
        {
            sStripTheme(*ptPlayer, *ptACD);
        }
    }

    // The text Config.txt would hold: one 'key "value"' line per buff.
    std::string sConfig;
    std::string sAll = sKeys + "," + D3_MOD_THEME_EVENT_KEYS;
    size_t nAt = 0;
    while (nAt < sAll.size())
    {
        size_t nEnd = sAll.find(',', nAt);
        std::string sName = sAll.substr(nAt, nEnd == std::string::npos ? std::string::npos : nEnd - nAt);
        nAt = nEnd == std::string::npos ? sAll.size() : nEnd + 1;
        if (!sName.empty())
        {
            sConfig += "CommunityBuff" + sName + " \"1\"\n";
        }
    }
    KeyValueMap mapKeyValues;
    if (!sConfig.empty())
    {
        std::string sData = sConfig;  // the parser works in place
        Prefs::ParseFileDataToKeyValues(&sData[0], (uint32)sData.size(), mapKeyValues);
    }

    CommunityBuffs & tBuffs = Console::Online::stCommunityBuffs;
    tBuffs.ResetAll();
    tBuffs.ReadConfig(mapKeyValues);
    // No buffs at all is off: 2.7.7 turns Dark Alchemy and Nesting Portals on by default
    // with any event, so the event itself stays off.
    BOOL bEvent = !sConfig.empty() && tBuffs.AnyActive();
    s_varCommunityEventPerkEnabled.Set(bEvent != FALSE, XVarBase::DIRTY_DEFAULT);
    s_varCommunityEventPerkSeasonOnly.Set(false, XVarBase::DIRTY_DEFAULT);
    tBuffs.StoreToCvars();

    for (Player * ptPlayer = PlayerGetFirstInGame(); ptPlayer != NULL; ptPlayer = PlayerGetNextInGame(ptPlayer))
    {
        if (ActorCommonData * ptACD = ACDTryToGet(ptPlayer->idPlayerACD))
        {
            SPlayerApplyPerkBuffs(*ptACD);
        }
    }
    fprintf(stderr, "theme: community buffs on: %s%s%s (event %s)\n", sKeys.empty() ? "none" : sKeys.c_str(),
            *D3_MOD_THEME_EVENT_KEYS ? ", events " : "", D3_MOD_THEME_EVENT_KEYS,
            bEvent ? "active" : "inactive");
}

// Switches to a request; bKeep stores it for the next start.
void sSwitch(const std::string & sRequest, bool bKeep)
{
    std::string sKeys;
    if (!sResolve(sRequest, sKeys))
    {
        fprintf(stderr, "theme: \"%s\" is not a season (14 to 37), off, default or buff names\n", sRequest.c_str());
        sWriteStatus("\"" + sTrim(sRequest) + "\" is not a theme.");
        return;
    }
    sApply(sKeys);
    sgsCurrent = sTrim(sRequest) == "default" ? std::string(D3_MOD_THEME_KEYS) : sTrim(sRequest);
    sgsCurrentKeys = sKeys;
    std::string sName = sDescribe(sgsCurrent == "Season" && D3_MOD_THEME_SEASON ? std::to_string(D3_MOD_THEME_SEASON) : sgsCurrent, sKeys);
    fprintf(stderr, "theme: now %s\n", sName.c_str());
    if (bKeep)
    {
        std::string sPath = sFolder() + "/theme.txt";
        if (sTrim(sRequest) == "default")
        {
            remove(sPath.c_str());
        }
        else
        {
            sWriteFile(sPath, sgsCurrent + "\n");
        }
    }
    sWriteStatus("Theme: " + sName + ".");
}

}  // namespace

// The designer menu (designer_menu.inc) asks whether the theme mod is built in.
bool D3ModThemeOn() { return true; }

// The designer menu's requests (any thread); the next server update applies them.
void D3ModThemeRequest(const char * szRequest)
{
    std::lock_guard<std::mutex> tLock(sgLock);
    sgsQueued = szRequest ? szRequest : "";
    if (sgsQueued.empty())
    {
        sgsQueued = "off";
    }
}

// Once per server player update (src/mods/hooks.cpp).
void D3ModThemeUpdate()
{
    static bool bStarted = false;
    static int nLogIn = 2;  // updates until the hero is described
    if (!bStarted)
    {
        bStarted = true;
        sWriteThemes();
        // The theme picked last time, else the build's.
        std::string sKept = sReadLine(sFolder() + "/theme.txt");
        std::string sKeys;
        if (!sKept.empty() && sResolve(sKept, sKeys))
        {
            fprintf(stderr, "theme: keeping the theme picked last time, %s\n", sKept.c_str());
            sSwitch(sKept, false);
        }
        else
        {
            if (D3_MOD_THEME_SEASON)
            {
                const SeasonTheme * ptTheme = sFindSeason(D3_MOD_THEME_SEASON);
                fprintf(stderr, "theme: season %d's theme is %s\n", D3_MOD_THEME_SEASON, ptTheme ? ptTheme->szKeys : "none");
            }
            sSwitch("default", false);
        }
    }

    std::string sRequest;
    {
        std::lock_guard<std::mutex> tLock(sgLock);
        sRequest.swap(sgsQueued);
    }
    std::string sPath = sFolder() + "/request.txt";
    std::string sTaken = sPath + ".taken";
    if (rename(sPath.c_str(), sTaken.c_str()) == 0)
    {
        sRequest = sReadLine(sTaken);
        remove(sTaken.c_str());
        if (sRequest.empty())
        {
            sRequest = "off";
        }
    }
    if (!sRequest.empty())
    {
        sSwitch(sRequest, true);
        nLogIn = 2;
    }

    // An update after each switch, for the first hero in a game: what the theme gave it.
    Player * ptPlayer = nLogIn > 0 ? PlayerGetFirstInGame() : NULL;
    ActorCommonData * ptACD = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    if (ptACD && --nLogIn == 0)
    {
        fprintf(stderr, "theme: hero: fourth cube slot %d, soul shards %d, ethereal %d, sanctified %d, dark alchemy %d\n",
                (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_FOURTH_KANAIS_CUBE_SLOT)),
                (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_SOUL_SHARDS_UNLOCKED)),
                (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_ETHEREAL_ITEMS_UNLOCKED)),
                (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_SANCTIFIED_ITEMS_UNLOCKED)),
                (int)ptACD->AttributesGetInt(FastAttribKey(ATTRIBUTE_DARK_ALCHEMY_POWERS_UNLOCKED)));
    }
}
