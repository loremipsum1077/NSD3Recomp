// The Corrupt mod (D3_MOD_CORRUPT): a "Corrupt" recipe in Kanai's Cube. A legendary item
// comes out carrying one boss skill, picked at random, which it casts at the enemy now
// and then when the hero hits something while wearing it.
//
// The recipe: 1 legendary item, 25 Death's Breath, 5 Forgotten Souls and 1 Corrupted
// Angel Flesh. Corrupting a corrupted item rerolls the skill.
//
// How it fits in:
//   - The corruption is an affix (D3Mod_Corrupt_<power>), added to the GameBalance affix
//     table at load. It has no attributes of its own; being an affix is what makes it
//     saved with the item. An item keeps at most 6 affixes through a save, so on an item
//     that has 6 the corruption takes the place of the last one.
//   - The recipe is a row added to the GameBalance transmute recipe table, with action 40
//     (the game's run to 18); SItemCrafting.cpp's sTransmuteExecuteAction calls
//     D3ModCorruptItem for it. Its title and text come from D3ModCorruptString: the cube
//     asks for the name of action 40, which the game has none for, as "[symbol not found]".
//   - PowerAPI.cpp calls D3ModCorruptOnHit wherever the game rolls its own on-hit effects.
//   - The console item tooltip calls D3ModCorruptTooltip after the item's properties.
// The table, string and update hooks are shared with other mods (src/mods/hooks.cpp).
//
// For testing: D3_CORRUPT_TRACE=1 logs what the mod does. D3_CORRUPT_TEST=powers casts
// every boss skill in turn from the hero at a zombie and logs the damage each did;
// D3_CORRUPT_TEST=item corrupts a legendary in the hero's backpack once and keeps a zombie
// beside the hero to hit; D3_CORRUPT_TEST=cube lists the hero's corrupted items and puts
// the hero beside the town's Kanai's Cube.
#include "X/Common/ActorCommonData.h"
#include "X/Common/ActorInventory.h"
#include "X/Common/Definitions/GameBalanceDefinition.h"
#include "X/Common/GameBalance.h"
#include "X/Common/GlobalSNO.h"
#include "X/Common/Items.h"
#include "X/Common/Player.h"
#include "X/Common/Protobuf/Items.pb.h"
#include "X/Common/SNO/SNOFiles.h"
#include "X/Common/SceneCommon.h"
#include "X/Common/Target.h"
#include "X/Common/UserInterface/FontDefinition.h"
#include "X/Common/UserInterface/UITextControl.h"
#include "X/Server/Actors/Actors.h"
#include "X/Server/LuaScript/ScriptUtil.h"
#include "X/Server/Powers/Powers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

BOOL ACDInventorySlotIsEquippable(InventorySlotType eSlot);  // X/Common/ActorInventory.cpp
BOOL ItemIsEquippableByPlayerOrHireling(GBID gbidItem);      // X/Common/Items.cpp
ACDID SItemPlayerReplace(ActorCommonData & tACDOldItem, D3::Items::Generator & tNewItemGenerator, Player & tPlayer, BOOL bResocketGems);  // X/Server/SItems.cpp

namespace {

enum { GB_AFFIXES_TYPE = 8 };  // GB_AFFIXES (not in the decomp's adopted subset)
const int kActionCorrupt = 40;
const int kMaxSavedAffixes = 6;         // CItemGenerate re-applies at most this many
const float kProcChance = 0.15f;        // per hit, times the hit's proc coefficient
const double kCooldownSeconds = 1.5;    // per corrupted item

struct BossSkill {
    const char * power;  // the boss's power, cast by the hero
    const char * boss;
    const char * skill;
};

// The skills an item can be corrupted with. Each is saved by its power's name, so the
// list can be reordered or added to; an item with a skill taken off it loses the
// corruption (and needs the affix to load, so keep removed names in kRetired).
const BossSkill kSkills[] = {
    {"Diablo_RingOfFire", "Diablo", "Ring of Fire"},
    {"Diablo_ExpandingFireRing", "Diablo", "Expanding Fire Ring"},
    {"Diablo_LightningBreath_v2", "Diablo", "Lightning Breath"},
    {"Diablo_FireMeteor", "Diablo", "Fire Meteor"},
    {"Diablo_HellSpikes", "Diablo", "Hell Spikes"},
    {"Diablo_StompAndStun", "Diablo", "Stomp"},
    {"Belial_LightningStrike_v2", "Belial", "Lightning Strike"},
    {"Belial_LightningBreath", "Belial", "Lightning Breath"},
    {"Belial_Ground_Pound_con", "Belial", "Ground Pound"},
    {"Azmodan_GlobeOfAnnihilation", "Azmodan", "Globe of Annihilation"},
    {"Azmodan_FallingCorpses", "Azmodan", "Rain of Corpses"},
    {"ZoltunKulle_EnergyTwister", "Zoltun Kulle", "Energy Twister"},
    {"ZoltunKulle_FieryBoulder", "Zoltun Kulle", "Fiery Boulder"},
    {"ZoltunKulle_SlowTime", "Zoltun Kulle", "Slow Time"},
    {"Izual_FrostNova", "Izual", "Frost Nova"},
    {"Maghda_MothDust", "Maghda", "Moth Dust"},
    {"Maghda_Projectile", "Maghda", "Arcane Bolt"},
    {"SkeletonKing_Whirlwind", "The Skeleton King", "Whirlwind"},
    {"SkeletonKing_Cleave", "The Skeleton King", "Cleave"},
    {"SiegebreakerDemon_Stomp", "Siegebreaker", "Stomp"},
    {"x1_Malthael_Mephisto_SkullMissile", "Malthael", "Mephisto's Skull Missile"},
    {"x1_Malthael_Baal_Hoarfrost", "Malthael", "Baal's Hoarfrost"},
    {"x1_Malthael_Mephisto_PoisonCloud", "Malthael", "Mephisto's Poison Cloud"},
    {"x1_Urzael_FlameSweep", "Urzael", "Flame Sweep"},
    {"x1_Urzael_Cannonball", "Urzael", "Cannonball"},
    {"SpiderQueen_WebSpit", "Araneae", "Web Spit"},
    {"Adria_event47_blast", "Adria", "Blast"},
};
const int kSkillCount = sizeof(kSkills) / sizeof(kSkills[0]);

// Skills that were on the list once: their affixes are still made so items carrying
// them load, but nothing gets them any more and they do nothing.
const char * const kRetired[] = {""};

GBID gAffix[kSkillCount];
SNO gPower[kSkillCount];
std::vector<AffixTableEntry> * gAffixes;
std::vector<TransmuteRecipe> * gRecipes;
std::vector<std::string> gNames;  // the affix rows' names, alive as long as the table

bool Trace()
{
    static bool on = getenv("D3_CORRUPT_TRACE") != NULL;
    return on;
}

const char * TestMode()
{
    static const char * mode = getenv("D3_CORRUPT_TEST");
    return mode ? mode : "";
}

std::string AffixName(const char * szPower)
{
    return std::string("D3Mod_Corrupt_") + szPower;
}

int SkillOfAffix(GBID gbid)
{
    for (int i = 0; i < kSkillCount; i++)
    {
        if (gAffix[i] == gbid && gbid != GBID_NONE)
        {
            return i;
        }
    }
    return -1;
}

bool IsCorruptAffix(GBID gbid)
{
    if (SkillOfAffix(gbid) >= 0)
    {
        return true;
    }
    for (const char * szRetired : kRetired)
    {
        if (*szRetired && gbid == StringToGBID(AffixName(szRetired).c_str()))
        {
            return true;
        }
    }
    return false;
}

int SkillOf(const ActorCommonData & tItem)
{
    POS pos = tItem.listIdentifiedAffixGBIDs.GetHeadPosition();
    while (pos)
    {
        int i = SkillOfAffix(tItem.listIdentifiedAffixGBIDs.GetNext(pos));
        if (i >= 0)
        {
            return i;
        }
    }
    return -1;
}

SNO PowerSNO(int i)
{
    if (gPower[i] == SNO_NONE || gPower[i] == 0)
    {
        gPower[i] = SNOFileLookupGUID(POWER_SNO, kSkills[i].power);
    }
    return gPower[i];
}

void AddAffixes(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable)
{
    AffixTableEntry * ptAffixes = (AffixTableEntry *)pTable;
    if (nCount <= 0)
    {
        return;
    }
    gAffixes = new std::vector<AffixTableEntry>(ptAffixes, ptAffixes + nCount);
    std::vector<std::string> arPowers;
    for (const BossSkill & tSkill : kSkills)
    {
        arPowers.push_back(tSkill.power);
    }
    for (const char * szRetired : kRetired)
    {
        if (*szRetired)
        {
            arPowers.push_back(szRetired);
        }
    }
    gNames.reserve(arPowers.size());  // the rows point into these strings
    for (const std::string & power : arPowers)
    {
        gNames.push_back(AffixName(power.c_str()));
        // A copy of the first affix with everything that would make it drop, roll or
        // change the item taken out.
        AffixTableEntry tAffix = ptAffixes[0];
        tAffix.tHeader.szName = (char *)gNames.back().c_str();
        tAffix.tHeader.gbid = StringToGBID(gNames.back().c_str());
        tAffix.nFrequencyWeight = 0;
        memset(tAffix.nClassFrequencyWeight, 0, sizeof tAffix.nClassFrequencyWeight);
        memset(tAffix.nHirelingFrequencyWeight, 0, sizeof tAffix.nHirelingFrequencyWeight);
        tAffix.gbidConvertsTo = GBID_NONE;
        tAffix.gbidLegendaryUprankAffix = GBID_NONE;
        tAffix.gbidAffixFamily0 = GBID_NONE;
        tAffix.gbidAffixFamily1 = GBID_NONE;
        tAffix.gbidExclusionCategory = GBID_NONE;
        tAffix.gbidAssociatedAffix = GBID_NONE;
        for (AttributeSpecifier & tSpec : tAffix.ptAffixAttributes)
        {
            tSpec.eAttribute = ATTRIBUTE_NONE;
        }
        for (AttributeSpecifier & tSpec : tAffix.ptRequirements)
        {
            tSpec.eAttribute = ATTRIBUTE_NONE;
        }
        gAffixes->push_back(tAffix);
    }
    for (int i = 0; i < kSkillCount; i++)
    {
        gAffix[i] = StringToGBID(AffixName(kSkills[i].power).c_str());
    }
    pdefGB->tAffixTable.ptAffixes = gAffixes->data();
    pdefGB->tAffixTable.serAffixes.dwSize = (uint32)(gAffixes->size() * sizeof(AffixTableEntry));
    nCount = (int32)gAffixes->size();
    pTable = gAffixes->data();
    if (Trace())
    {
        fprintf(stderr, "corrupt: added %d affixes\n", (int)arPowers.size());
    }
}

void AddRecipe(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable)
{
    TransmuteRecipe * ptRecipes = (TransmuteRecipe *)pTable;
    if (nCount <= 0)
    {
        return;
    }
    gRecipes = new std::vector<TransmuteRecipe>(ptRecipes, ptRecipes + nCount);
    int32 nLastPage = 0;
    for (int i = 0; i < nCount; i++)
    {
        nLastPage = ptRecipes[i].nPage > nLastPage ? ptRecipes[i].nPage : nLastPage;
        if (Trace())
        {
            const TransmuteRecipe & r = ptRecipes[i];
            const char * szRecipe = r.tHeader.szName;
            fprintf(stderr, "corrupt: recipe %s action %d page %d hidden %d:", szRecipe ? szRecipe : "?", r.eAction, r.nPage, r.bHidden);
            for (int k = 0; k < r.nIngredientsCount && k < 8; k++)
            {
                fprintf(stderr, " [%d %d x%d %d %d]", r.tIngredients[k].eType, r.tIngredients[k].nTypeValue, r.tIngredients[k].nQuantity,
                        r.tIngredients[k].unk_0c, r.tIngredients[k].unk_10);
            }
            fprintf(stderr, "\n");
        }
    }
    TransmuteRecipe tRecipe = ptRecipes[0];
    static const char szName[] = "D3Mod_Corrupt";
    tRecipe.tHeader.szName = (char *)szName;
    tRecipe.tHeader.gbid = StringToGBID(szName);
    tRecipe.eAction = kActionCorrupt;
    tRecipe.nPage = nLastPage + 1;
    tRecipe.bHidden = FALSE;
    TransmuteRecipeIngredient arIngredients[] = {
        {TRIT_LEGENDARY, -1, 1, -1, -1},
        {TRIT_SPECIFIC_GBID, (int32)StringToGBID("Crafting_Looted_Reagent_01"), 25, -1, -1},  // Death's Breath
        {TRIT_SPECIFIC_GBID, (int32)StringToGBID("Crafting_Legendary_01"), 5, -1, -1},        // Forgotten Soul
        {TRIT_SPECIFIC_GBID, (int32)StringToGBID("p2_ActBountyReagent_04"), 1, -1, -1},       // Corrupted Angel Flesh
    };
    memset(tRecipe.tIngredients, 0, sizeof tRecipe.tIngredients);
    for (TransmuteRecipeIngredient & tIngredient : tRecipe.tIngredients)
    {
        tIngredient.eType = TRIT_INVALID;
        tIngredient.unk_0c = tIngredient.unk_10 = -1;
    }
    tRecipe.nIngredientsCount = (int32)(sizeof arIngredients / sizeof arIngredients[0]);
    for (int k = 0; k < tRecipe.nIngredientsCount; k++)
    {
        tRecipe.tIngredients[k] = arIngredients[k];
    }
    gRecipes->push_back(tRecipe);
    pdefGB->tTransmuteRecipeTable.ptRecipes = gRecipes->data();
    pdefGB->tTransmuteRecipeTable.serRecipes.dwSize = (uint32)(gRecipes->size() * sizeof(TransmuteRecipe));
    nCount = (int32)gRecipes->size();
    pTable = gRecipes->data();
}

const InventoryLocation * Held(const ActorCommonData * ptACD)
{
    return ptACD ? ptACD->TryToGetInventoryLocation() : NULL;
}

bool WornBy(const ActorCommonData & tItem, ACDID idHero)
{
    const InventoryLocation * ptAt = Held(&tItem);
    if (!ptAt || ptAt->idACDContainer != idHero)
    {
        return false;
    }
    if (strcmp(TestMode(), "item") == 0 && ptAt->eSlot == SLOT_PLAYER_BACKPACK)
    {
        return true;  // so the test needn't equip it
    }
    return ACDInventorySlotIsEquippable(ptAt->eSlot) != FALSE;
}

// The corrupted items each hero wears, found once a second.
struct Worn {
    ACDID idItem;
    ACDID idHero;
    int nSkill;
};
std::vector<Worn> gWorn;
std::map<ACDID, std::chrono::steady_clock::time_point> gLastProc;

void FindWorn()
{
    gWorn.clear();
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        ActorCommonData * ptItem = tIt.ptElement;
        if (!ptItem || !ptItem->IsItem())
        {
            continue;
        }
        int i = SkillOf(*ptItem);
        const InventoryLocation * ptAt = i >= 0 ? Held(ptItem) : NULL;
        if (ptAt && ACDIsPlayer(ptAt->idACDContainer) && WornBy(*ptItem, ptAt->idACDContainer))
        {
            gWorn.push_back({ptItem->id, ptAt->idACDContainer, i});
        }
    }
}

bool IsBossPower(SNO snoPower)
{
    for (int i = 0; i < kSkillCount; i++)
    {
        if (snoPower != SNO_NONE && PowerSNO(i) == snoPower)
        {
            return true;
        }
    }
    return false;
}

// The hero casts the skill at the target, without an animation or a cost.
bool Cast(ActorCommonData & tHero, ActorCommonData & tTarget, int i)
{
    SNO snoPower = PowerSNO(i);
    if (snoPower == SNO_NONE)
    {
        return false;
    }
    PowerStartData tStart;
    PowerStartDataInit(tStart);
    tStart.idACD = tHero.id;
    tStart.snoPower = snoPower;
    tStart.tTarget.InitializeFromACD(tTarget.id, FALSE, FALSE);
    tStart.bFreeCast = TRUE;
    tStart.bDoesntAnimate = TRUE;
    tStart.bAllowHitProcs = FALSE;
    return PowerStartEx(tStart, -1, TRUE, TRUE) != POWERID_INVALID;
}

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

float Life(const ActorCommonData & tACD)
{
    return tACD.AttributesGetFloat(FastAttribKey(ATTRIBUTE_HITPOINTS_CUR));
}

// D3_CORRUPT_TEST=powers: each skill in turn, cast from the hero at a fresh zombie, with
// the zombie's life before and two seconds after.
void TestPowers(ActorCommonData & tHero)
{
    static int nSkill = 0, nStep = 0;
    static ACDID idTarget = ACDID_INVALID;
    static float flBefore = 0.0f;
    static bool bCast = false;
    static SNO snoTarget = SNOFileLookupGUID(ACTOR_SNO, "ZombieSkinny_A");
    if (nSkill >= kSkillCount)
    {
        return;
    }
    tHero.SetTakesNoDamage(TRUE);
    ActorCommonData * ptTarget = ACDTryToGet(idTarget);
    switch (nStep)
    {
    case 0:
        idTarget = Spawn(snoTarget, tHero, 10.0f, 0.0f);
        nStep = 1;
        break;
    case 1:
        if (ptTarget)
        {
            flBefore = Life(*ptTarget);
            bCast = Cast(tHero, *ptTarget, nSkill);
        }
        nStep = 2;
        break;
    case 2:
        nStep = 3;
        break;
    case 3:
        fprintf(stderr, "corrupt: test %-36s cast %d  life %.0f -> %s\n", kSkills[nSkill].power, bCast, flBefore,
                ptTarget && ptTarget->IsAlive() ? std::to_string((int)Life(*ptTarget)).c_str() : "dead");
        if (ptTarget && ptTarget->IsAlive())
        {
            ActorKill(idTarget, (DeathType)0, FALSE, FALSE, tHero.id);
        }
        nSkill++;
        nStep = 0;
        break;
    }
}

// D3_CORRUPT_TEST=item: corrupt the first legendary in the backpack, once, the way the
// cube would.
void TestItem(Player & tPlayer, ActorCommonData & tHero);

}  // namespace

BOOL D3ModCorruptItem(Player & tPlayer, const ACDID * arItems, int32 nItems, ACDID & idKeep, ACDID & idNew, GameError & eError);

void D3ModCorruptExtendTable(GameBalanceDefinition * pdefGB, int32 & nCount, void *& pTable)
{
    if (pdefGB->eGameBalanceType == GB_AFFIXES_TYPE && !gAffixes)
    {
        AddAffixes(pdefGB, nCount, pTable);
    }
    else if (pdefGB->eGameBalanceType == GB_TRANSMUTE_RECIPES && !gRecipes)
    {
        AddRecipe(pdefGB, nCount, pTable);
    }
}

// The cube asks for the recipe's title and text under the name of its action, which the
// game has no name for: "[symbol not found]". Only the corrupt recipe has such an action.
const char * D3ModCorruptString(SNO snoStringList, const char * szLabel)
{
    if (!szLabel || szLabel[0] != '[' || strncmp(szLabel, "[symbol not found]", 18) != 0)
    {
        return NULL;
    }
    if (snoStringList == GlobalSNOGet(GLOBALSNO_STRINGLIST_TRANSMUTE_RECIPES))
    {
        return "Corrupt Legendary";
    }
    if (snoStringList == GlobalSNOGet(GLOBALSNO_STRINGLIST_TRANSMUTE_FLAVOR_TEXT))
    {
        return "Bind the power of a fallen lord into a legendary item. When you hit an enemy, it may "
               "unleash that boss's skill upon them. The boss is chosen at random; corrupting the item "
               "again chooses anew. If the item has no room, the corruption takes one of its properties.";
    }
    return NULL;
}

BOOL D3ModCorruptItem(Player & tPlayer, const ACDID * arItems, int32 nItems, ACDID & idKeep, ACDID & idNew, GameError & eError)
{
    ActorCommonData * ptItem = NULL;
    for (int32 i = 0; i < nItems && !ptItem; i++)
    {
        ActorCommonData * ptCandidate = ACDTryToGet(arItems[i]);
        if (ptCandidate && ItemIsLegendary(ptCandidate->hGB.gbid) && ItemIsEquippableByPlayerOrHireling(ptCandidate->hGB.gbid))
        {
            ptItem = ptCandidate;
        }
    }
    if (!ptItem || gAffix[0] == GBID_NONE)
    {
        eError = GAMEERROR_TRANSMUTE_FAILED_CANNOT_FIND_ITEM_TO_UPGRADE;
        return FALSE;
    }
    int nOld = SkillOf(*ptItem);
    int nSkill = (int)arc4random_uniform((uint32)kSkillCount);
    if (kSkillCount > 1 && nSkill == nOld)
    {
        nSkill = (nSkill + 1 + (int)arc4random_uniform((uint32)kSkillCount - 1)) % kSkillCount;
    }

    D3::Items::Generator tGenerator;
    ItemGeneratorPopulateFromACDID(*ptItem, tGenerator, TRUE);
    std::vector<int32> arAffixes;
    for (int i = 0; i < tGenerator.base_affixes_size(); i++)
    {
        if (!IsCorruptAffix(tGenerator.base_affixes(i)))
        {
            arAffixes.push_back(tGenerator.base_affixes(i));
        }
    }
    if ((int)arAffixes.size() >= kMaxSavedAffixes)
    {
        arAffixes.resize(kMaxSavedAffixes - 1);
    }
    arAffixes.push_back((int32)gAffix[nSkill]);
    tGenerator.clear_base_affixes();
    for (int32 gbid : arAffixes)
    {
        tGenerator.add_base_affixes(gbid);
    }

    idKeep = ptItem->id;
    idNew = SItemPlayerReplace(*ptItem, tGenerator, tPlayer, FALSE);
    if (Trace())
    {
        fprintf(stderr, "corrupt: %s -> %s (%s), new item %d\n", GBIDToString(ptItem->hGB.gbid, GB_ITEMS, FALSE).str(), kSkills[nSkill].power,
                nOld >= 0 ? "rerolled" : "new", (int)idNew);
    }
    if (idNew == ACDID_INVALID)
    {
        eError = GAMEERROR_TRANSMUTE_FAILED;
        return FALSE;
    }
    return TRUE;
}

void D3ModCorruptTooltip(const ActorCommonData & tItem, CRefString & sText);

namespace {
void TestItem(Player & tPlayer, ActorCommonData & tHero)
{
    static bool done = false;
    static ACDID idTarget = ACDID_INVALID;
    if (done)
    {
        // Something to hit: a zombie beside the hero, replaced when it dies.
        static SNO snoTarget = SNOFileLookupGUID(ACTOR_SNO, "ZombieSkinny_A");
        tHero.SetTakesNoDamage(TRUE);
        ActorCommonData * ptTarget = ACDTryToGet(idTarget);
        if (!ptTarget || !ptTarget->IsAlive())
        {
            idTarget = Spawn(snoTarget, tHero, 4.0f, 0.0f);
        }
        return;
    }
    ACDIterator tIt;
    while (ActorCommonDataIterate(tIt))
    {
        ActorCommonData * ptItem = tIt.ptElement;
        const InventoryLocation * ptAt = ptItem && ptItem->IsItem() ? Held(ptItem) : NULL;
        if (ptAt && ptAt->idACDContainer == tHero.id && ptAt->eSlot == SLOT_PLAYER_BACKPACK && ItemIsLegendary(ptItem->hGB.gbid) &&
            ItemIsEquippableByPlayerOrHireling(ptItem->hGB.gbid))
        {
            done = true;
            ACDID idKeep, idNew;
            GameError eError = 0;
            BOOL bOK = D3ModCorruptItem(tPlayer, &ptItem->id, 1, idKeep, idNew, eError);
            // SItemPlayerReplace has already put the new item in the old one's place.
            fprintf(stderr, "corrupt: test item: %d (error %d)\n", bOK, eError);
            if (ActorCommonData * ptNew = ACDTryToGet(idNew))
            {
                CRefString sTooltip;
                ::D3ModCorruptTooltip(*ptNew, sTooltip);
                fprintf(stderr, "corrupt: test item affixes %d, tooltip: %s", (int)ptNew->listIdentifiedAffixGBIDs.Count(), sTooltip.str());
            }
            return;
        }
    }
}
}  // namespace

void D3ModCorruptOnHit(ActorCommonData & tAttacker, ActorCommonData & tTarget, float flProcScalar, SNO snoPower)
{
    static bool busy = false;
    if (busy || gWorn.empty() || !tTarget.IsMonster() || !tTarget.IsAlive() || IsBossPower(snoPower))
    {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    for (const Worn & tWorn : gWorn)
    {
        if (tWorn.idHero != tAttacker.id)
        {
            continue;
        }
        auto it = gLastProc.find(tWorn.idItem);
        if (it != gLastProc.end() && std::chrono::duration<double>(now - it->second).count() < kCooldownSeconds)
        {
            continue;
        }
        float flChance = kProcChance * (flProcScalar > 0.0f ? flProcScalar : 1.0f);
        if (arc4random_uniform(10000) >= (uint32)(flChance * 10000.0f))
        {
            continue;
        }
        gLastProc[tWorn.idItem] = now;
        busy = true;
        bool bCast = Cast(tAttacker, tTarget, tWorn.nSkill);
        busy = false;
        if (Trace())
        {
            fprintf(stderr, "corrupt: proc %s: %d\n", kSkills[tWorn.nSkill].power, bCast);
        }
    }
}

// One orange line under the item's properties: which boss's skill it carries.
void D3ModCorruptTooltip(const ActorCommonData & tItem, CRefString & sText)
{
    int i = SkillOf(tItem);
    if (i < 0)
    {
        return;
    }
    CRefString sLine;
    sLine.Format("{icon:bullet} Corrupted: hitting an enemy may unleash %s's %s.\n", kSkills[i].boss, kSkills[i].skill);
    Set_Text_Color(sLine, FONTCOLOR_ORANGE);
    sText += sLine;
}

// The tooltip's patch declares the hook inside GetItemDescription, which is in this
// namespace.
namespace UIInventory { namespace Console {
void D3ModCorruptTooltip(const ActorCommonData & tItem, CRefString & sText)
{
    ::D3ModCorruptTooltip(tItem, sText);
}
} }

void D3ModCorruptUpdate()
{
    FindWorn();
    if (!*TestMode())
    {
        return;
    }
    Player * ptPlayer = PlayerGetFirstInGame();
    ActorCommonData * ptHero = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    if (!ptHero)
    {
        return;
    }
    if (strcmp(TestMode(), "powers") == 0)
    {
        TestPowers(*ptHero);
    }
    else if (strcmp(TestMode(), "item") == 0)
    {
        TestItem(*ptPlayer, *ptHero);
    }
    else if (strcmp(TestMode(), "cube") == 0)
    {
        // Once in town: list the hero's corrupted items (to see they were saved) and
        // put the hero beside Kanai's Cube.
        static bool done = false;
        static SNO snoCube = SNOFileLookupGUID(ACTOR_SNO, "KanaiCube_Stand");
        ACDIterator tIt;
        ActorCommonData * ptCube = NULL;
        int nCorrupted = 0;
        while (ActorCommonDataIterate(tIt))
        {
            ActorCommonData * ptACD = tIt.ptElement;
            if (!ptACD)
            {
                continue;
            }
            if (ptACD->snoActor == snoCube && ptACD->tLocation.tWorldLocation.idSWorld == ptHero->tLocation.tWorldLocation.idSWorld)
            {
                ptCube = ptACD;
            }
            const InventoryLocation * ptAt = ptACD->IsItem() ? Held(ptACD) : NULL;
            if (!done && ptAt && ptAt->idACDContainer == ptHero->id && SkillOf(*ptACD) >= 0)
            {
                nCorrupted++;
                fprintf(stderr, "corrupt: test cube: hero holds %s with %s (slot %d)\n", GBIDToString(ptACD->hGB.gbid, GB_ITEMS, FALSE).str(),
                        kSkills[SkillOf(*ptACD)].power, (int)ptAt->eSlot);
            }
        }
        if (!done && ptCube)
        {
            done = true;
            const auto & c = ptCube->tLocation.tWorldLocation.transform.wp;
            const auto & h = ptHero->tLocation.tWorldLocation.transform.wp;
            fprintf(stderr, "corrupt: test cube: %d corrupted items; warping from %.0f,%.0f to the cube at %.0f,%.0f\n", nCorrupted, h.x, h.y, c.x, c.y);
            ActorWarpToActor(ptHero->id, ptCube->id);
        }
        else if (done)
        {
            static int nLogged = 0;
            if (nLogged++ < 3)
            {
                const auto & h = ptHero->tLocation.tWorldLocation.transform.wp;
                fprintf(stderr, "corrupt: test cube: hero now at %.0f,%.0f\n", h.x, h.y);
            }
        }
    }
}
