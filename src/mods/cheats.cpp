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
// The game answers in <save>/cheats/status.json and lists every item, with its in-game
// name, in <save>/cheats/items.json for the page's item picker.
#include "X/Common/ActorCommonData.h"
#include "X/Common/Definitions/GameBalanceDefinition.h"
#include "X/Common/Items.h"
#include "X/Common/Player.h"
#include "X/Server/LuaScript/ScriptUtil.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

BOOL ExperienceGrantDirectly(ActorCommonData & tACDPlayer, const uint64 nExpGranted, const BOOL bNotifyClient,
                             const BOOL bGrantToAllPlayersIfLocal);  // X/Server/Experience.cpp

extern "C" const char * d3_save_dir(void);  // src/nn/fs/fs.cpp
extern "C" int d3_hotkeys_take(void);       // src/host/window.cpp
enum { D3_HOTKEY_LEVEL_MAX = 1 << 0, D3_HOTKEY_PARAGON = 1 << 1, D3_HOTKEY_GOLD = 1 << 2 };

namespace {

const int kHotkeyParagonLevels = 100;
const int kMaxItemCount = 50;
const long long kHotkeyGold = 10000000;
const long long kMaxGold = 100000000000LL;

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

void Run(const char * szLine, ACDID idACDPlayer, ActorCommonData & tACD)
{
    char szWord[32] = {};
    char szName[128] = {};
    int nValue = 0;
    long long nGold = 0;
    if (sscanf(szLine, "gold %lld", &nGold) == 1)
    {
        AddGold(tACD, nGold);
    }
    else if (sscanf(szLine, "level %d", &nValue) == 1)
    {
        RaiseLevel(tACD, nValue);
    }
    else if (sscanf(szLine, "paragon %d", &nValue) == 1)
    {
        AddParagon(tACD, std::max(0, std::min(nValue, 20000)));
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

}  // namespace

void D3ModCheatsUpdate()
{
    static bool listed;
    static unsigned updates;
    if (!listed)
    {
        listed = true;
        WriteItemList();
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
                    Run(szLine, ptPlayer->idPlayerACD, *ptACD);
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
