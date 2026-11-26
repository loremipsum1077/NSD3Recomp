// The Cain mod (D3_MOD_VENDOR): Deckard Cain stands in New Tristram again and sells
// custom legendary gems.
//
// Each gem is a copy of a real legendary gem (its look, icon and own power are kept), at
// its highest rank, with a new name, new text and one extra power that this file runs:
// every N kills while the gem is socketed in something the hero wears, the hero gets a
// pylon's buff. The copies are added to the game's item table as it loads, under names of
// their own, so the game saves them with the hero like any other item; a hero holding
// one must be loaded with this mod on.
//
// The decomp is patched in six places (cmake/DecompSources.cmake):
//   ActorInventory.cpp         sGetInitialInventoryType  D3ModVendorIsMerchantActor: a shop
//   GameBalanceDefinition.cpp  sLoadGBInfo           D3ModVendorExtendItems: add the copies
//   StringListDefinition.cpp   sStringListFetchInternal  D3ModVendorString: their names, text
//   NPC.cpp                    NPCCanTrade           D3ModVendorIsVendor: Cain trades
//   ScriptEvent.cpp            ScriptEventOnActorKilled  D3ModVendorOnKill: count kills
//   SPlayer.cpp                SPlayersUpdate (1/s)  D3ModVendorUpdate: spawn and restock Cain
//
// For testing: D3_VENDOR_TRACE=1 logs what the mod does; D3_VENDOR_WARP=1 puts the hero
// beside Cain once he appears; D3_VENDOR_TEST=1 gives the hero a Conduit Shard that counts
// from the backpack and feeds the hero zombies to kill (see Test below).
#include "X/Common/ActorCommonData.h"
#include "X/Common/ActorInventory.h"
#include "X/Common/Actors/ActorEffectCommon.h"
#include "X/Common/Definitions/GameBalanceDefinition.h"
#include "X/Common/Definitions/LevelAreaDefinition.h"
#include "X/Common/GameBalance.h"
#include "X/Common/GlobalSNO.h"
#include "X/Common/Items.h"
#include "X/Common/Player.h"
#include "X/Common/SNO/SNOFiles.h"
#include "X/Common/SceneCommon.h"
#include "X/Server/Actors/Actors.h"
#include "X/Server/LuaScript/ScriptUtil.h"
#include "X/Server/Loot/Loot.h"
#include "X/Server/SItems.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

BOOL ACDInventorySlotIsEquippable(InventorySlotType eSlot);                       // X/Common/ActorInventory.cpp
void ACDRemoveFromCurrentPosition(ActorCommonData & tACDItem);                    // X/Common/ActorInventory.cpp
void NPCUpdateConversationIconAndInteraction(ActorCommonData & tACDNPC, const Player & tPlayer);  // X/Server/SNPC.cpp
// X/Server/SItems.cpp. 2.7.7 added the owner, used only to replace items whose definition
// has flag bit 30 (CMake sets D3_VENDOR_UPGRADE_HAS_OWNER from the decomp's source).
#if D3_VENDOR_UPGRADE_HAS_OWNER
BOOL SItemUpgradeJewel(ActorCommonData & tACDItem, int32 nUpgradeRanks, ActorCommonData & tACDOwner);
#else
void SItemUpgradeJewel(ActorCommonData & tACDItem, int32 nUpgradeRanks);
#endif

namespace {

struct CustomGem {
    const char * name;      // the new GameBalance name (saved with the hero)
    const char * base;      // the legendary gem it copies
    const char * display;   // its name in game
    const char * text;      // shown under it, in gold
    int kills;              // every this many kills...
    const char * power;     // ...this power is used on the hero
    int price;              // gold
};

// The gems Cain sells. Add more here: any legendary gem as the base, any buff power.
const CustomGem kGems[] = {
    {"D3Mod_Gem_Conduit", "Unique_Gem_021_x1", "Conduit Shard",
     "Every 10 kills, you are charged as if by a Conduit Pylon.", 10, "Pages_Buff_Electrified", 1000000},
    {"D3Mod_Gem_Power", "Unique_Gem_001_x1", "Shard of Power",
     "Every 25 kills, you are empowered as if by a Power Pylon.", 25, "Pages_Buff_Damage", 1000000},
    {"D3Mod_Gem_Speed", "Unique_Gem_008_x1", "Quickening Shard",
     "Every 10 kills, you are hastened as if by a Speed Pylon.", 10, "Pages_Buff_Run_Speed", 1000000},
};
const int kGemCount = sizeof kGems / sizeof kGems[0];
const int kDefaultMaxRank = 100;

GBID gGemGBID[kGemCount];
ACDID gCain = ACDID_INVALID;
std::map<std::pair<ACDID, int>, int> gKills;  // (hero, gem) -> kills since the last buff

// D3_VENDOR_TEST=1 is for scripted tests of the kill powers: the hero gets a Conduit
// Shard, gems in the backpack count too, and zombies keep coming to the hero in town.
bool TestMode()
{
    static int test = getenv("D3_VENDOR_TEST") != NULL;
    return test;
}

bool Trace()
{
    static int trace = getenv("D3_VENDOR_TRACE") != NULL;
    return trace;
}

// Where an item is held, or NULL when it is not in an inventory.
const InventoryLocation * Held(const ActorCommonData * ptACD)
{
    return ptACD ? ptACD->TryToGetInventoryLocation() : NULL;
}

int GemIndex(GBID gbid)
{
    for (int i = 0; i < kGemCount; i++)
    {
        if (gGemGBID[i] == gbid && gbid != GBID_NONE)
        {
            return i;
        }
    }
    return -1;
}

ActorCommonData * FindInWorld(SNO snoActor, SWorldID idWorld)
{
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        ActorCommonData * ptACD = tIt.ptElement;
        if (ptACD && ptACD->snoActor == snoActor && ptACD->tLocation.tWorldLocation.idSWorld == idWorld)
        {
            return ptACD;
        }
    }
    return NULL;
}

SNO CainSNO()
{
    static SNO sno = SNOFileLookupGUID(ACTOR_SNO, "Cain");
    return sno;
}

// New Tristram in the story, and the same town in adventure mode.
bool InTown(const ActorCommonData & tACD)
{
    static SNO snoStory = SNOFileLookupGUID(gptGroupDefLevelArea->GetGroup(), "A1_trOUT_NewTristram");
    static SNO snoAdventure = SNOFileLookupGUID(gptGroupDefLevelArea->GetGroup(), "A1_Tristram_Adventure_Mode_Hub");
    SNO snoArea = ACDGetLevelArea(tACD);
    return snoArea == snoStory || snoArea == snoAdventure;
}

// Where Cain stands, from the town's waypoint, in world units: -y is up and to the left on
// screen, clear of the banners, which stand up and to the right (-x).
const float kCainOffsetX = 2.0f;
const float kCainOffsetY = -16.0f;

// Like the game's own script spawn (sActorSpawn in ScriptUtil.cpp), at a set spot.
ACDID Spawn(SNO snoActor, const ActorCommonData & tACDNear, float flDX, float flDY)
{
    ActorAddStruct tActorAdd;
    tActorAdd.tSNOName.snoGroup = ACTOR_SNO;
    tActorAdd.tSNOName.snoHandle = snoActor;
    tActorAdd.snoActor = snoActor;
    tActorAdd.hGB.gbid = snoActor;
    tActorAdd.hGB.eType = (GameBalanceType)-1;
    tActorAdd.eMonsterRarity = (MonsterRarity)0;
    tActorAdd.tPlace.idSWorld = tACDNear.tLocation.tWorldLocation.idSWorld;
    tActorAdd.tPlace.wp = tACDNear.tLocation.tWorldLocation.transform.wp;
    tActorAdd.tPlace.wp.x += flDX;
    tActorAdd.tPlace.wp.y += flDY;
    tActorAdd.qOrientation = tACDNear.tLocation.tWorldLocation.transform.q;
    tActorAdd.dwActorAddFlags |= 1 << ACTORADD_DONT_PERTURB;
    ScenesCommonNavMeshNodeFromPlace(tActorAdd.tPlace, &tActorAdd.tNavMeshHandle, FALSE, FALSE);
    ActorID idActor = GBAdd(tActorAdd);
    return idActor != ACTORID_INVALID ? ACDFindByActor(idActor) : ACDID_INVALID;
}

// Cain stands near New Tristram's waypoint whenever a hero is in town.
ActorCommonData * Cain(ActorCommonData & tACDHero)
{
    ActorCommonData * ptCain = ACDTryToGet(gCain);
    if (ptCain && ptCain->snoActor == CainSNO())
    {
        return ptCain;
    }
    gCain = ACDID_INVALID;
    if (!InTown(tACDHero))
    {
        return NULL;
    }
    SWorldID idWorld = tACDHero.tLocation.tWorldLocation.idSWorld;
    SNO snoCain = CainSNO();
    if (ActorCommonData * ptExisting = FindInWorld(snoCain, idWorld))
    {
        gCain = ptExisting->id;
    }
    else
    {
        ActorCommonData * ptWaypoint = FindInWorld(SNOFileLookupGUID(ACTOR_SNO, "Waypoint_Town"), idWorld);
        if (!ptWaypoint)
        {
            return NULL;
        }
        gCain = Spawn(snoCain, *ptWaypoint, kCainOffsetX, kCainOffsetY);
        if (Trace())
        {
            ActorCommonData * ptNew = ACDTryToGet(gCain);
            fprintf(stderr, "vendor: spawned Cain as %x at %.1f,%.1f (waypoint %.1f,%.1f)\n", (unsigned)gCain,
                    ptNew ? ptNew->GetPlace().wp.x : 0.0f, ptNew ? ptNew->GetPlace().wp.y : 0.0f,
                    ptWaypoint->GetPlace().wp.x, ptWaypoint->GetPlace().wp.y);
        }
    }
    ptCain = ACDTryToGet(gCain);
    return ptCain && ACDInventoryGetType(*ptCain) == INVENTORY_MERCHANT ? ptCain : NULL;
}

void Stock(ActorCommonData & tCain, int nLevel)
{
    if (!tCain.ptInventory)
    {
        return;
    }
    bool have[kGemCount] = {};
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        ActorCommonData * ptACD = tIt.ptElement;
        const InventoryLocation * ptAt = Held(ptACD);
        if (ptAt && ptACD->IsItem() && ptAt->idACDContainer == tCain.id)
        {
            int i = GemIndex(ptACD->hGB.gbid);
            if (i >= 0)
            {
                have[i] = true;
            }
        }
    }
    for (int i = 0; i < kGemCount; i++)
    {
        if (have[i] || gGemGBID[i] == GBID_NONE)
        {
            continue;
        }
        LootSpecifier tSpecifier;
        tSpecifier.hGBItem.Set(GB_ITEMS, gGemGBID[i]);
        tSpecifier.snoActorForFlippy = GameBalanceGetActorSNO(tSpecifier.hGBItem);
        tSpecifier.fAutoIdentify = TRUE;
        tSpecifier.nAffixLevel = nLevel;
        LootDetermineInherentAffixes(tSpecifier);
        tSpecifier.eIQL = ItemDetermineQualityLevel("Cain mod", tSpecifier, tSpecifier.nNumAffixes);
        tSpecifier.nLegendaryItemLevel = nLevel;
        ACDID idItem = LootCreateItem(tSpecifier, cgplaceNull, &tCain);
        ActorCommonData * ptItem = ACDTryToGet(idItem);
        if (!ptItem)
        {
            continue;
        }
        PickupResponse eResult = SACDInventoryPickup(tCain.id, idItem, EFFECT_INVALID, FALSE, FALSE, NULL, FALSE, FALSE, FALSE, FALSE);
        if (eResult != PICKUP_RESPONSE_OK)
        {
            ACDRemoveFromCurrentPosition(*ptItem);
            ActorDelete((ActorID)ACDGetOwner(idItem));
            continue;
        }
        // Raise it to its highest rank the way Urshi does, so its power scales and its
        // second effect unlocks. In 2.7.7 the upgrade's last step replaces an item whose
        // definition has flag bit 30 for its player owner; Cain is no player, so those
        // (no legendary gem should be one) stay as they are.
        int nMaxRank;
        bool bReplaced;
        {
            ScopedItemDefinitionLockR pdefItem(gGemGBID[i]);
            nMaxRank = pdefItem != NULL ? pdefItem->nJewelMaxRank : 0;
            bReplaced = pdefItem == NULL || ((pdefItem->dwFlags >> 30) & 1);
        }
        int nRank = nMaxRank > 0 ? nMaxRank : kDefaultMaxRank;
        int nNow = ptItem->AttributesGetInt(FastAttribKey(ATTRIBUTE_JEWEL_RANK));
        if (nRank > nNow && !bReplaced)
        {
#if D3_VENDOR_UPGRADE_HAS_OWNER
            SItemUpgradeJewel(*ptItem, nRank - nNow, tCain);
#else
            SItemUpgradeJewel(*ptItem, nRank - nNow);
#endif
        }
        if (Trace())
        {
            fprintf(stderr, "vendor: stocked %s, max rank %d, now rank %d\n", kGems[i].name, nMaxRank,
                    ptItem->AttributesGetInt(FastAttribKey(ATTRIBUTE_JEWEL_RANK)));
        }
    }
}

// The custom gem copies, kept for the life of the program (the table points at them).
std::vector<ItemDefinition> * gTable;

}  // namespace

// Called as the game registers each game-balance table's rows; adds the copies once, to
// the table that holds the first base gem.
void D3ModVendorExtendItems(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable)
{
    if (gTable || pdefGB->eGameBalanceType != GB_ITEMS)
    {
        return;
    }
    ItemDefinition * ptItems = (ItemDefinition *)pTable;
    int nBase[kGemCount];
    bool any = false;
    for (int g = 0; g < kGemCount; g++)
    {
        nBase[g] = -1;
        for (int i = 0; i < nCount; i++)
        {
            if (ptItems[i].tHeader.szName && strcmp(ptItems[i].tHeader.szName, kGems[g].base) == 0)
            {
                nBase[g] = i;
                any = true;
                break;
            }
        }
    }
    if (!any)
    {
        return;
    }
    gTable = new std::vector<ItemDefinition>(ptItems, ptItems + nCount);
    for (int g = 0; g < kGemCount; g++)
    {
        if (nBase[g] < 0)
        {
            fprintf(stderr, "vendor: no base item %s for %s\n", kGems[g].base, kGems[g].name);
            continue;
        }
        ItemDefinition tGem = ptItems[nBase[g]];
        tGem.tHeader.szName = (char *)kGems[g].name;
        tGem.tHeader.gbid = StringToGBID(kGems[g].name);
        tGem.nBaseCostGold = kGems[g].price;
        tGem.nLegendaryDropWeight = 0;
        gGemGBID[g] = tGem.tHeader.gbid;
        gTable->push_back(tGem);
    }
    pdefGB->tItemTable.ptItems = gTable->data();
    pdefGB->tItemTable.serItems.dwSize = (uint32)(gTable->size() * sizeof(ItemDefinition));
    nCount = (int32)gTable->size();
    pTable = gTable->data();
}

// The custom gems' names, and their text: the game fetches an item's flavor text from its
// own string list under the item's name.
const char * D3ModVendorString(SNO snoStringList, const char * szLabel)
{
    if (!szLabel || strncmp(szLabel, "D3Mod_", 6) != 0)
    {
        return NULL;
    }
    for (const CustomGem & tGem : kGems)
    {
        if (strcmp(szLabel, tGem.name) == 0)
        {
            return snoStringList == GlobalSNOGet(GLOBALSNO_STRINGLIST_ITEM_FLAVOR) ? tGem.text : tGem.display;
        }
    }
    return NULL;
}

// Cain in New Tristram trades, and is made with a merchant's inventory. Both the server's
// and the client's copies of him ask, so this goes by who and where he is, not his id.
BOOL D3ModVendorIsVendor(const ActorCommonData & tACD)
{
    return tACD.snoActor == CainSNO() && InTown(tACD);
}

// Asked as any actor is made, before it is placed: every Cain gets a merchant's (empty)
// inventory, which only matters for the one in town.
BOOL D3ModVendorIsMerchantActor(const ActorCommonData & tACD)
{
    return tACD.snoActor == CainSNO();
}

namespace {

// Whether a custom gem counts for this hero: socketed in something the hero wears (or,
// in test mode, anywhere in the hero's backpack).
bool Worn(const ActorCommonData & tGem, ACDID idHero)
{
    const InventoryLocation * ptAt = Held(&tGem);
    if (!ptAt)
    {
        return false;
    }
    if (TestMode() && ptAt->idACDContainer == idHero && ptAt->eSlot == SLOT_PLAYER_BACKPACK)
    {
        return true;
    }
    if (ptAt->eSlot < SLOT_SOCKETS_START || ptAt->eSlot >= SLOT_SOCKETS_END)
    {
        return false;
    }
    const InventoryLocation * ptHostAt = Held(ACDTryToGet(ptAt->idACDContainer));
    return ptHostAt && ptHostAt->idACDContainer == idHero && ACDInventorySlotIsEquippable(ptHostAt->eSlot);
}

// Test mode: a Conduit Shard in the backpack once, an unhurtable hero, and two zombies
// beside the hero each second, which the hero is credited with killing a second later.
void Test(ActorCommonData & tHero)
{
    static bool given = false;
    if (!given && gGemGBID[0] != GBID_NONE)
    {
        given = true;
        LootSpecifier tSpecifier;
        tSpecifier.hGBItem.Set(GB_ITEMS, gGemGBID[0]);
        tSpecifier.snoActorForFlippy = GameBalanceGetActorSNO(tSpecifier.hGBItem);
        tSpecifier.fAutoIdentify = TRUE;
        tSpecifier.nAffixLevel = 70;
        LootDetermineInherentAffixes(tSpecifier);
        tSpecifier.eIQL = ItemDetermineQualityLevel("Cain mod test", tSpecifier, tSpecifier.nNumAffixes);
        tSpecifier.nLegendaryItemLevel = 70;
        ACDID idItem = LootCreateItem(tSpecifier, cgplaceNull, &tHero);
        PickupResponse eResult = SACDInventoryPickup(tHero.id, idItem, EFFECT_INVALID, TRUE, FALSE, NULL, FALSE, FALSE, FALSE, FALSE);
        fprintf(stderr, "vendor: test gem to backpack: %d\n", (int)eResult);
    }
    static SNO snoZombie = SNOFileLookupGUID(ACTOR_SNO, "ZombieSkinny_A");
    tHero.SetTakesNoDamage(TRUE);
    std::vector<ACDID> arZombies;
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        if (tIt.ptElement && tIt.ptElement->snoActor == snoZombie && tIt.ptElement->IsAlive())
        {
            arZombies.push_back(tIt.ptElement->id);
        }
    }
    for (ACDID idZombie : arZombies)
    {
        ActorKill(idZombie, (DeathType)0, FALSE, FALSE, tHero.id);
    }
    Spawn(snoZombie, tHero, 6.0f, 6.0f);
    Spawn(snoZombie, tHero, -6.0f, 6.0f);
}

}  // namespace

// Each kill counts for every custom gem the killer wears; every so many, its power fires.
void D3ModVendorOnKill(ACDID idACDVictim, ACDID idACDKiller)
{
    ActorCommonData * ptVictim = ACDTryToGet(idACDVictim);
    ActorCommonData * ptHero = ACDTryToGet(idACDKiller);
    if (!ptVictim || !ptVictim->IsMonster() || !ptHero || !ACDIsPlayer(ptHero->id))
    {
        return;
    }
    bool counted[kGemCount] = {};  // two of a kind count once
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        ActorCommonData * ptGem = tIt.ptElement;
        int i = ptGem && ptGem->IsItem() ? GemIndex(ptGem->hGB.gbid) : -1;
        if (i < 0 || counted[i] || !Worn(*ptGem, ptHero->id))
        {
            continue;
        }
        counted[i] = true;
        int & nKills = gKills[std::make_pair(ptHero->id, i)];
        nKills++;
        if (Trace())
        {
            fprintf(stderr, "vendor: %s kill %d of %d\n", kGems[i].name, nKills, kGems[i].kills);
        }
        if (nKills >= kGems[i].kills)
        {
            nKills = 0;
            ActorTriggerPowerAtFeet(ptHero->id, kGems[i].power);
            if (Trace())
            {
                fprintf(stderr, "vendor: %s used %s\n", kGems[i].name, kGems[i].power);
            }
        }
    }
}

void D3ModVendorUpdate()
{
    Player * ptPlayer = PlayerGetFirstInGame();
    ActorCommonData * ptHero = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    if (!ptHero)
    {
        return;
    }
    if (ActorCommonData * ptCain = Cain(*ptHero))
    {
        // The server works out whether an NPC can be talked to when conversations change;
        // nothing changes for Cain, so ask it here until it says yes.
        if (!ptCain->AttributesGetInt(MakeAttribKey(ATTRIBUTE_NPC_HAS_INTERACT_OPTIONS, ptPlayer->nPlayerIndex)))
        {
            NPCUpdateConversationIconAndInteraction(*ptCain, *ptPlayer);
            if (Trace())
            {
                fprintf(stderr, "vendor: Cain npc %d operatable %d interact %d\n", ptCain->AttributesGetInt(FastAttribKey(ATTRIBUTE_IS_NPC)),
                        ptCain->AttributesGetInt(FastAttribKey(ATTRIBUTE_NPC_IS_OPERATABLE)),
                        ptCain->AttributesGetInt(MakeAttribKey(ATTRIBUTE_NPC_HAS_INTERACT_OPTIONS, ptPlayer->nPlayerIndex)));
            }
        }
        // For scripted tests: D3_VENDOR_WARP=1 puts the hero beside Cain once.
        static bool warp = getenv("D3_VENDOR_WARP") != NULL;
        if (warp)
        {
            warp = false;
            ActorWarpToActor(ptHero->id, ptCain->id);
        }
        Stock(*ptCain, ptHero->AttributesGetInt(FastAttribKey(ATTRIBUTE_LEVEL)));
        if (TestMode())
        {
            Test(*ptHero);
        }
    }
}
