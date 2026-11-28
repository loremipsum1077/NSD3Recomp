// The Cheats mod (D3_MOD_CHEATS): level the hero, add paragon levels and give items, for
// testing. Compiled into the game only when the mod is on; the decomp's SGame.cpp is
// patched to call D3ModCheatsUpdate() once per server update, right after the game's own
// Lua scripts run, so everything here happens on the server's thread through the same
// functions quest scripts use (ExperienceGrantDirectly, ItemGiveToPlayer).
//
// Requests come from two places:
//   F1 / F2 / F3 in the game window level to the cap / add 100 paragon levels / add 10
//                                   million gold
//   <save>/cheats/commands.txt      written by the Mods page (tools/mods.py), one per line:
//       level <n>          raise the hero to level n (at most the level cap)
//       paragon <n>        add n paragon levels (needs the level cap)
//       item <name> [n]    give n (default 1) of the item with that GameBalance name
//       gold <n>           add n gold
//       cache <act> [n]    give n (default 1) Horadric Caches for act 1-5 at the game's
//                          difficulty
//       cube               unlock Kanai's Cube (saved with the account, like picking it
//                          up in the Ruins of Sescheron)
//       brawl              go where the brawler in New Tristram sends heroes: the dueling
//                          world, or with the Loose Files mod one of the 1.0 arenas
//       spawn <actor> [normal|champion|rare|boss]
//                          add a monster near the hero by its actor's SNO name, e.g.
//                          "spawn X1_LR_Boss_MorluSpellcaster_Ice boss": Rime, the ice
//                          Morlu rift guardian the BlizzCon 2017 rift's encounter names
// The game answers in <save>/cheats/status.json and lists every item, with its in-game
// name, in <save>/cheats/items.json for the page's item picker.
#include "X/Common/ActorCommonData.h"
#include "X/Common/Definitions/GameBalanceDefinition.h"
#include "X/Common/Items.h"
#include "X/Common/Player.h"
#include "X/Common/Crafting/ItemCrafting.h"
#include "X/Common/Crafting/ItemCraftingSwig.h"
#include "X/Common/Game.h"
#include "X/Server/Crafting/SItemCrafting.h"
#include "X/Server/LuaScript/ScriptUtil.h"
#include "X/Server/Actors/Actors.h"
#include "X/Common/Monsters.h"
#include "X/Common/SNO/SNOFiles.h"
#include "X/Common/GameTemplate.h"
#include "X/Sigma/DataStructures/XVar.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

BOOL ExperienceGrantDirectly(ActorCommonData & tACDPlayer, const uint64 nExpGranted, const BOOL bNotifyClient,
                             const BOOL bGrantToAllPlayersIfLocal);  // X/Server/Experience.cpp

ActorID GBAddWithEncounterAddStruct(ActorAddStruct & tActorAdd, EncounterAddStruct & tEncounterAdd);   // X/Server/Actors/Actors.cpp
BOOL SPlayerFindGoodSpawnLocation(const WorldPlace & placeStart, WorldPlace & placeResult, float flSearchRadius);  // X/Server/SPlayer.cpp
BOOL ScenesCommonNavMeshNodeFromPlace(const WorldPlace & tPlace, NavMeshHandle * ptNavMeshHandle, const BOOL fCanActivateScenes, const BOOL fProcessInactive);  // X/Common/SceneCommon.cpp

extern XVarBool s_varGameDisableAreaDamage;     // X/Server/SGame.cpp
extern XVarBool s_varGameDisableDOTAreaDamage;  // X/Server/SGame.cpp

extern "C" const char * d3_save_dir(void);  // src/nn/fs/fs.cpp
extern "C" int d3_hotkeys_take(void);       // src/host/window.cpp
enum { D3_HOTKEY_LEVEL_MAX = 1 << 0, D3_HOTKEY_PARAGON = 1 << 1, D3_HOTKEY_GOLD = 1 << 2 };

namespace {

const int kHotkeyParagonLevels = 100;
const int kMaxItemCount = 50;
const long long kHotkeyGold = 10000000;
const long long kMaxGold = 100000000000LL;
const int kMaxCaches = 20;

std::string Folder()
{
    std::string folder = std::string(d3_save_dir()) + "/cheats";
    mkdir(d3_save_dir(), 0755);
    mkdir(folder.c_str(), 0755);
    return folder;
}

std::string Json(const char * text)
{
    std::string out = "\"";
    for (const unsigned char * p = (const unsigned char *)text; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            out += '\\';
            out += (char)*p;
        }
        else if (*p < 0x20)
        {
            char escaped[8];
            snprintf(escaped, sizeof escaped, "\\u%04x", *p);
            out += escaped;
        }
        else
        {
            out += (char)*p;
        }
    }
    return out + "\"";
}

// Written to a temporary file and renamed, so the page never reads half a file.
void WriteFile(const std::string & path, const std::string & text)
{
    std::string temporary = path + ".tmp";
    FILE * file = fopen(temporary.c_str(), "wb");
    if (!file)
    {
        return;
    }
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    rename(temporary.c_str(), path.c_str());
}

std::vector<std::string> gLog;

void Say(const std::string & line)
{
    fprintf(stderr, "cheats: %s\n", line.c_str());
    gLog.push_back(line);
    if (gLog.size() > 20)
    {
        gLog.erase(gLog.begin());
    }
}

// ItemDefinition::eDisplayedQualityLevel is set on few items; a set item names its set,
// and a legendary has a legendary drop weight or a legendary family.
const char * Rarity(const ItemDefinition & tItem)
{
    if (tItem.gbidSet != GBID_NONE && tItem.gbidSet != INVALID_GBID)
    {
        return "set";
    }
    if (tItem.nLegendaryDropWeight > 0 || (tItem.gbidLegendaryFamily != GBID_NONE && tItem.gbidLegendaryFamily != INVALID_GBID) ||
        tItem.eDisplayedQualityLevel == IQL_LEGENDARY)
    {
        return "legendary";
    }
    return "";
}

void WriteItemList()
{
    GBHandleList listItems(GBGetHandlePool());
    GBEnumerate(GB_ITEMS, listItems);
    struct Row { std::string name, display, type, rarity; bool drops; };
    std::vector<Row> rows;
    for (POS pos = listItems.GetHeadPosition(); pos;)
    {
        const GBHandle & hItem = listItems.GetNext(pos);
        ScopedItemDefinitionLockR pdefItem(hItem.gbid);
        if (pdefItem == NULL)
        {
            continue;
        }
        Row row;
        row.name = GBIDToString(hItem.gbid, GB_ITEMS, FALSE).str();
        row.display = ItemGetDisplayName(hItem.gbid).str();
        row.type = GBIDToString(pdefItem->gbidItemType, (GameBalanceType)1 /* GB_ITEMTYPES */, FALSE).str();
        row.rarity = Rarity(*pdefItem);
        // Old copies of a legendary (from before 2.0) are kept in the data but never drop.
        row.drops = pdefItem->nLegendaryDropWeight > 0;
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const Row & a, const Row & b) {
        return a.display != b.display ? a.display < b.display : a.name < b.name;
    });
    std::string text = "[\n";
    for (size_t i = 0; i < rows.size(); i++)
    {
        text += "  {\"name\": " + Json(rows[i].name.c_str()) + ", \"display\": " + Json(rows[i].display.c_str()) +
                ", \"type\": " + Json(rows[i].type.c_str()) + ", \"rarity\": " + Json(rows[i].rarity.c_str()) +
                ", \"drops\": " + (rows[i].drops ? "true" : "false") +
                (i + 1 < rows.size() ? "},\n" : "}\n");
    }
    WriteFile(Folder() + "/items.json", text + "]\n");
    Say("listed " + std::to_string(rows.size()) + " items");
}

int Level(ActorCommonData & tACD)
{
    return tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_LEVEL));
}

int Paragon(ActorCommonData & tACD)
{
    return tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_ALT_LEVEL));
}

// One level's worth of experience at a time, through the game's own level-up path, so
// skills, attributes and paragon points come out as if the levels had been earned.
void RaiseLevel(ActorCommonData & tACD, int nTarget)
{
    int nCap = tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_LEVEL_CAP));
    nTarget = std::min(nTarget, nCap);
    int nStart = Level(tACD);
    while (Level(tACD) < nTarget && !tACD.ShouldUseAltLevels())
    {
        if (!ExperienceGrantDirectly(tACD, tACD.GetExperienceNext(), Level(tACD) + 1 == nTarget, FALSE))
        {
            break;
        }
    }
    Say("level " + std::to_string(nStart) + " -> " + std::to_string(Level(tACD)));
}

void AddParagon(ActorCommonData & tACD, int nLevels)
{
    if (!tACD.ShouldUseAltLevels())
    {
        Say("paragon levels need the level cap first (F1 or \"level 70\")");
        return;
    }
    int nStart = Paragon(tACD);
    for (int i = 0; i < nLevels; i++)
    {
        if (!ExperienceGrantDirectly(tACD, tACD.GetAltExperienceNext(), i + 1 == nLevels, FALSE))
        {
            break;
        }
    }
    Say("paragon " + std::to_string(nStart) + " -> " + std::to_string(Paragon(tACD)));
}

void AddGold(ActorCommonData & tACD, long long nGold)
{
    nGold = std::max(0LL, std::min(nGold, kMaxGold));
    tACD.ModifyCurrencyAmount(nGold, CURRENCY_GOLD, GMR_SCRIPT, FALSE);
    Say("gold +" + std::to_string(nGold) + " -> " + std::to_string((long long)tACD.GetCurrencyAmount(CURRENCY_GOLD)));
}

// Greater rifts: count solo greater rift nLevel as cleared by this hero, which opens the
// tiers up to it at the obelisk. When the hero is next saved, the account is marked as
// having cleared a solo greater rift, which is what unlocks Challenge Rifts.
void SetRiftLevel(ActorCommonData & tACD, int nLevel)
{
    nLevel = std::max(1, std::min(nLevel, 150));
    tACD.AttributesSetInt(FastAttribKey(ATTRIBUTE_HIGHEST_SOLO_RIFT_LEVEL), std::max(nLevel, (int)tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_HIGHEST_SOLO_RIFT_LEVEL))));
    tACD.AttributesSetInt(FastAttribKey(ATTRIBUTE_HIGHEST_HERO_SOLO_RIFT_LEVEL), std::max(nLevel, (int)tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_HIGHEST_HERO_SOLO_RIFT_LEVEL))));
    tACD.AttributesSetInt(FastAttribKey(ATTRIBUTE_HIGHEST_UNLOCKED_RIFT_LEVEL), std::max(nLevel, (int)tACD.AttributesGetInt(FastAttribKey(ATTRIBUTE_HIGHEST_UNLOCKED_RIFT_LEVEL))));
    Say("greater rift " + std::to_string(nLevel) + " counted as cleared solo (opens Challenge Rifts once saved)");
}

// Horadric Caches are one item per act and difficulty: HoradricCacheA1, A1Hard,
// A1Expert, A1Master, A1Torment (Torment 1) and A1Torment2 to A1Torment13. The data has
// none past Torment 13, so Torment 14-16 get the Torment 13 cache.
std::string CacheName(int nAct, int nHandicap)
{
    static const char * const kNamed[] = {"", "Hard", "Expert", "Master", "Torment"};
    std::string name = "HoradricCacheA" + std::to_string(nAct);
    if (nHandicap < 0)
    {
        nHandicap = 0;
    }
    if (nHandicap <= 4)
    {
        return name + kNamed[nHandicap];
    }
    return name + "Torment" + std::to_string(std::min(nHandicap - 3, 13));
}

void GiveCaches(ACDID idACDPlayer, int nAct, int nCount)
{
    if (nAct < 1 || nAct > 5)
    {
        Say("cache: the act must be 1 to 5");
        return;
    }
    std::string name = CacheName(nAct, GameGetHandicapLevel());
    int nGiven = 0;
    for (int i = 0; i < nCount; i++)
    {
        if (ItemGiveToPlayer(idACDPlayer, name.c_str(), NULL, NULL, NULL, NULL, NULL, NULL) != ACDID_INVALID)
        {
            nGiven++;
        }
    }
    Say("gave " + std::to_string(nGiven) + " x " + name + " (" + ItemGetDisplayName(StringToGBID(name.c_str())).str() + ")");
}

// The cube is the Horadrim crafter: it is unlocked once its crafter level is above 0.
void UnlockCube(Player & tPlayer)
{
    if (GetCraftLevel(CRAFTER_TYPE_HORADRIM, tPlayer) > 0)
    {
        Say("Kanai's Cube is already unlocked");
        return;
    }
    SCrafterUnlock(tPlayer, CRAFTER_TYPE_HORADRIM);
    Say(GetCraftLevel(CRAFTER_TYPE_HORADRIM, tPlayer) > 0 ? "unlocked Kanai's Cube" : "could not unlock Kanai's Cube (it needs adventure mode)");
}

void GiveItem(ACDID idACDPlayer, const char * szName, int nCount)
{
    int nGiven = 0;
    for (int i = 0; i < nCount; i++)
    {
        if (ItemGiveToPlayer(idACDPlayer, szName, NULL, NULL, NULL, NULL, NULL, NULL) != ACDID_INVALID)
        {
            nGiven++;
        }
    }
    if (nGiven)
    {
        Say("gave " + std::to_string(nGiven) + " x " + szName + " (" + ItemGetDisplayName(StringToGBID(szName)).str() + ")");
    }
    else
    {
        Say(std::string("no item named ") + szName);
    }
}

// A monster near the hero, by actor name, as the world boss mod adds its bosses.
void Spawn(ActorCommonData & tHero, const char * szName, const char * szRarity)
{
    SNOName tName;
    Team eTeam = (Team)10;  // TEAM_MONSTER_0
    tName.snoGroup = ACTOR_SNO;
    tName.snoHandle = SNOFileLookupGUID(ACTOR_SNO, szName);
    if (tName.snoHandle == SNO_NONE)
    {
        Say(std::string("no actor named ") + szName);
        return;
    }
    MonsterRarity eRarity = MR_NORMAL;
    if (strcmp(szRarity, "champion") == 0)
        eRarity = MR_CHAMPION;
    else if (strcmp(szRarity, "rare") == 0)
        eRarity = MR_RARE;
    else if (strcmp(szRarity, "boss") == 0)
        eRarity = MR_BOSS;

    WorldPlace tPlace = tHero.GetPlace();
    tPlace.wp.x += 15.0f;
    WorldPlace tFound;
    if (SPlayerFindGoodSpawnLocation(tPlace, tFound, 20.0f))
    {
        tPlace = tFound;
    }
    ActorAddStruct tActorAdd;
    tActorAdd.tSNOName = tName;
    tActorAdd.snoActor = tName.snoHandle;
    tActorAdd.tPlace = tPlace;
    tActorAdd.eMonsterRarity = eRarity;
    tActorAdd.eTeam = eTeam;
    tActorAdd.dwActorAddFlags |= (1 << ACTORADD_EVEN_WHEN_STUCK);
    tActorAdd.dwActorAddFlags |= (1 << ACTORADD_DONT_PERTURB);
    ScenesCommonNavMeshNodeFromPlace(tActorAdd.tPlace, &tActorAdd.tNavMeshHandle, FALSE, FALSE);
    GBHandleList listGroupAffixes(GBGetHandlePool());
    EncounterAddStruct tEncounterAdd;
    EncounterAddInitialize(tEncounterAdd, listGroupAffixes);
    ActorID idActor = GBAddWithEncounterAddStruct(tActorAdd, tEncounterAdd);
    if (idActor == ACTORID_INVALID || !ACDTryToGet(ACDFindByActor(idActor)))
    {
        Say(std::string("the game would not add ") + szName);
        return;
    }
    CRefString sMonster;
    if (!MonsterGetBaseName(tName, sMonster))
    {
        sMonster = SNOToString(ACTOR_SNO, tName.snoHandle);
    }
    Say(std::string("spawned ") + sMonster.str() + " (" + SNOToString(ACTOR_SNO, tName.snoHandle).str() + ")");
}

void Run(const char * szLine, Player & tPlayer, ACDID idACDPlayer, ActorCommonData & tACD)
{
    char szRarity[16] = {};
    char szWord[32] = {};
    char szName[128] = {};
    int nValue = 0;
    long long nGold = 0;
    int nAct = 0;
    if (strcmp(szLine, "cube") == 0)
    {
        UnlockCube(tPlayer);
    }
    else if (strcmp(szLine, "brawl") == 0)
    {
        // As the brawler's script (DuelNPC) does once his conversation ends; 10 is the
        // reason the scripts' three-argument call passes.
        PlayerWarpToWorld(idACDPlayer, "PVP_Duel_Small", "DuelWorld_Entrance", 0.0f, (PlayerWarpReason)10);
        Say("sent to the dueling world");
    }
    else if (sscanf(szLine, "cache %d %d", &nAct, &nValue) >= 1)
    {
        GiveCaches(idACDPlayer, nAct, std::max(1, std::min(nValue ? nValue : 1, kMaxCaches)));
    }
    else if (sscanf(szLine, "gold %lld", &nGold) == 1)
    {
        AddGold(tACD, nGold);
    }
    else if (sscanf(szLine, "level %d", &nValue) == 1)
    {
        RaiseLevel(tACD, nValue);
    }
    else if (sscanf(szLine, "rifts %d", &nValue) == 1)
    {
        SetRiftLevel(tACD, nValue);
    }
    else if (sscanf(szLine, "paragon %d", &nValue) == 1)
    {
        AddParagon(tACD, std::max(0, std::min(nValue, 20000)));
    }
    else if (sscanf(szLine, "spawn %127s %15s", szName, szRarity) >= 1)
    {
        Spawn(tACD, szName, szRarity);
    }
    else if (sscanf(szLine, "item %127s %d", szName, &nValue) >= 1)
    {
        GiveItem(idACDPlayer, szName, std::max(1, std::min(nValue ? nValue : 1, kMaxItemCount)));
    }
    else if (sscanf(szLine, "%31s", szWord) == 1)
    {
        Say(std::string("unknown command: ") + szLine);
    }
}

void WriteStatus(ActorCommonData * ptACD)
{
    std::string text = "{\n  \"in_game\": " + std::string(ptACD ? "true" : "false");
    if (ptACD)
    {
        text += ",\n  \"level\": " + std::to_string(Level(*ptACD)) + ",\n  \"paragon\": " + std::to_string(Paragon(*ptACD)) +
                ",\n  \"gold\": " + std::to_string((long long)ptACD->GetCurrencyAmount(CURRENCY_GOLD));
    }
    text += ",\n  \"updated\": " + std::to_string((long long)time(NULL)) + ",\n  \"log\": [";
    for (size_t i = 0; i < gLog.size(); i++)
    {
        text += (i ? ", " : "") + Json(gLog[i].c_str());
    }
    WriteFile(Folder() + "/status.json", text + "]\n}\n");
}

// Game rules (the Mods page's "Game rules" panel): rules.txt in the cheats folder holds
// lines "NAME on" or "NAME off"; a rule it does not name keeps the game type's own value.
// NAME is a GameParamFlags flag without its GPF_ prefix, or one of the server's switches.
struct Rule
{
    const char * szName;
    int nFlag;          // a GameParamFlags bit, or -1
    XVarBool * ptVar;   // or a server switch
};
const Rule kRules[] = {
    {"ADJUST_EXP_WITH_RELATIVE_LEVEL", GPF_ADJUST_EXP_WITH_RELATIVE_LEVEL, NULL},
    {"RESET_LIFE_STATS_UPON_LEVEL_UP", GPF_RESET_LIFE_STATS_UPON_LEVEL_UP, NULL},
    {"GRANT_GOLD_UPON_KILL", GPF_GRANT_GOLD_UPON_KILL, NULL},
    {"RESURRECTION_DELAY", GPF_RESURRECTION_DELAY, NULL},
    {"APPLY_RESURRECTION_BUFFS", GPF_APPLY_RESURRECTION_BUFFS, NULL},
    {"ALLOW_TOWN_PORTALS", GPF_ALLOW_TOWN_PORTALS, NULL},
    {"RESTOCK_SHOPS_UPON_TALK", GPF_RESTOCK_SHOPS_UPON_TALK, NULL},
    {"KEEP_ALL_SCENES_ACTIVE", GPF_KEEP_ALL_SCENES_ACTIVE, NULL},
    {"QUEST_UI", GPF_QUEST_UI, NULL},
    {"REVEAL_MINIMAP", GPF_REVEAL_MINIMAP, NULL},
    {"CREATE_HEADSTONES_UPON_PLAYER_DEATH", GPF_CREATE_HEADSTONES_UPON_PLAYER_DEATH, NULL},
    {"NO_EXPERIENCE", GPF_NO_EXPERIENCE, NULL},
    {"SHOW_MONSTER_HP_BARS", GPF_SHOW_MONSTER_HP_BARS, NULL},
    {"LIMIT_WEAPON_EFFECTS", GPF_LIMIT_WEAPON_EFFECTS, NULL},
    {"IGNORE_DURABILITY_LOSS", GPF_IGNORE_DURABILITY_LOSS, NULL},
    {"NO_AREA_DAMAGE", -1, &s_varGameDisableAreaDamage},
    {"NO_DOT_AREA_DAMAGE", -1, &s_varGameDisableDOTAreaDamage},
};

std::atomic<unsigned long long> gRulesOn(0), gRulesOff(0);

void ReadRules()
{
    static std::string seen = "\x01";  // nothing read yet
    std::string text;
    if (FILE * file = fopen((Folder() + "/rules.txt").c_str(), "rb"))
    {
        char buffer[512];
        size_t got;
        while ((got = fread(buffer, 1, sizeof buffer, file)) > 0 && text.size() < 8192)
        {
            text.append(buffer, got);
        }
        fclose(file);
    }
    if (text == seen)
    {
        return;
    }
    seen = text;
    unsigned long long on = 0, off = 0;
    std::string summary;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        char szName[64] = {}, szState[8] = {};
        if (sscanf(line.c_str(), "%63s %7s", szName, szState) != 2)
        {
            continue;
        }
        for (const Rule & tRule : kRules)
        {
            if (strcmp(tRule.szName, szName) != 0 || (strcmp(szState, "on") != 0 && strcmp(szState, "off") != 0))
            {
                continue;
            }
            if (tRule.nFlag >= 0)
            {
                (strcmp(szState, "on") == 0 ? on : off) |= 1ULL << tRule.nFlag;
            }
            summary += std::string(summary.empty() ? "" : ", ") + szName + " " + szState;
        }
    }
    for (const Rule & tRule : kRules)
    {
        if (tRule.ptVar)
        {
            tRule.ptVar->Set(summary.find(std::string(tRule.szName) + " on") != std::string::npos, 0);
        }
    }
    gRulesOn = on;
    gRulesOff = off;
    Say("game rules: " + (summary.empty() ? std::string("the game's own") : summary));
}

std::mutex gQueueLock;
std::vector<std::string> gQueue;  // from the designer menu (src/mods/designer_menu.inc)

}  // namespace

// GameIsParamFlagSet (X/Common/Game.cpp, patched) asks for the game type's flags through
// this, from any thread.
unsigned long long D3ModRulesApply(unsigned long long uFlags)
{
    return (uFlags | gRulesOn.load(std::memory_order_relaxed)) & ~gRulesOff.load(std::memory_order_relaxed);
}

// Any thread: run a command line at the next server update.
void D3ModCheatsQueue(const char * szLine)
{
    std::lock_guard<std::mutex> lock(gQueueLock);
    gQueue.push_back(szLine);
}

void D3ModCheatsUpdate()
{
    static bool listed;
    static unsigned updates;
    if (!listed)
    {
        listed = true;
        WriteItemList();
    }

    if (updates % 30 == 0)
    {
        ReadRules();
    }

    Player * ptPlayer = PlayerGetFirstInGame();
    ActorCommonData * ptACD = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    if (!ptACD)
    {
        if (++updates % 120 == 0)
        {
            WriteStatus(NULL);
        }
        return;
    }

    bool changed = false;
    int hotkeys = d3_hotkeys_take();
    if (hotkeys & D3_HOTKEY_LEVEL_MAX)
    {
        RaiseLevel(*ptACD, 70);
        changed = true;
    }
    if (hotkeys & D3_HOTKEY_GOLD)
    {
        AddGold(*ptACD, kHotkeyGold);
        changed = true;
    }
    if (hotkeys & D3_HOTKEY_PARAGON)
    {
        AddParagon(*ptACD, kHotkeyParagonLevels);
        changed = true;
    }

    std::vector<std::string> queued;
    {
        std::lock_guard<std::mutex> lock(gQueueLock);
        queued.swap(gQueue);
    }
    for (const std::string & line : queued)
    {
        Run(line.c_str(), *ptPlayer, ptPlayer->idPlayerACD, *ptACD);
        changed = true;
    }

    // The page's requests, checked twice a second.
    if (++updates % 30 == 0)
    {
        std::string path = Folder() + "/commands.txt";
        std::string taken = path + ".taken";
        if (rename(path.c_str(), taken.c_str()) == 0)
        {
            if (FILE * file = fopen(taken.c_str(), "rb"))
            {
                char szLine[256];
                while (fgets(szLine, sizeof szLine, file))
                {
                    szLine[strcspn(szLine, "\r\n")] = 0;
                    Run(szLine, *ptPlayer, ptPlayer->idPlayerACD, *ptACD);
                }
                fclose(file);
            }
            remove(taken.c_str());
            changed = true;
        }
    }
    if (changed || updates % 120 == 0)
    {
        WriteStatus(ptACD);
    }
}
