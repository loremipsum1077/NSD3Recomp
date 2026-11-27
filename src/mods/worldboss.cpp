// World bosses (D3_MOD_EVENTS=WorldBoss): every few minutes, wherever the hero is (town too,
// but not a rift), the game announces that a world boss waits in a zone of the hero's act,
// one with a waypoint. Nothing is loaded or spawned then. When the hero enters that zone, a
// spot in it, away from the hero, is marked on the minimap; the boss, one of the rift
// guardians, appears when the hero reaches the spot and the game has loaded that part of the
// map. It then shows on the minimap as a quest monster, and its death is announced too. A boss
// nobody comes for leaves after a while, and the next one is announced later.
//
// The pieces are the game's own:
//   the boss     the rift guardian encounter (GLOBALSNO_ENCOUNTER_DUNGEON_FINDER_BOSS), the
//                pool SWorldDungeonFinderSpawnBoss picks a rift's guardian from;
//   the zone     a waypoint of the hero's act (ActGetCachedWaypointArray), not a town, one
//                the hero has reached where the game keeps track;
//   the spot     a random point in a random scene of that zone, away from the hero;
//                the scene's specification names its area without loading it. On arrival
//                it is snapped to walkable ground with SPlayerFindGoodSpawnLocation, as town
//                portals do;
//   the markers  before the boss, an SMSG_MAP_MARKER with the event-location icon, as
//                Minimap::AddQuestMarker makes for bounty events; after, the boss's
//                ATTRIBUTE_QUEST_MONSTER (RegisterQuestActor), which the minimap draws as a
//                hostile marker, and SPlayerTrickleAdd, which keeps it on the map from afar;
//   the text     DisplayGameMessageForAllPlayers and its chat twin, with labels whose text
//                D3ModWorldBossString supplies (through the shared string hook).
//
// D3_MOD_WORLD_BOSS_MINUTES (CMake) sets the time between bosses; D3_WORLD_BOSS_SECONDS
// overrides it when starting the game, for testing, D3_WORLD_BOSS_TEST=1 puts the boss in
// the hero's own zone (even a town), and D3_WORLD_BOSS_NEAR=1 also puts the spot close to the hero so the boss wakes at
// once, and D3_WORLD_BOSS_SPOT=x,y picks the spot. Each step is logged as "worldboss: ..."; D3_WORLD_BOSS_TRACE=1 also logs each time
// the client fetches an announcement's text.
#include "Contrib/Contrib/blz/include/blz/vector.h"
#include "X/Common/Act.h"
#include "X/Common/ActorCommonData.h"
#include "X/Common/Definitions/ActDefinition.h"
#include "X/Common/Definitions/GameBalanceDefinition.h"
#include "X/Common/Definitions/LevelAreaDefinition.h"
#include "X/Common/Definitions/WorldDefinition.h"
#include "X/Common/GlobalSNO.h"
#include "X/Common/Marker/Marker.h"
#include "X/Common/Monsters.h"
#include "X/Common/Networking/GameMessage.h"
#include "X/Common/Player.h"
#include "X/Common/SceneCommon.h"
#include "X/Common/World.h"
#include "X/Server/Actors/Actors.h"
#include "X/Server/LuaScript/ScriptUtil.h"
#include "X/Server/PlayerList.h"
#include "X/Server/SPlayer.h"
#include "X/Sigma/Random.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Declared where the game defines them, as the decomp's own files do.
BOOL EncounterResolveToActorSpawnInfo(SNO snoEncounter, RNG & rng, SNOName & tSNOName, Team & eTeam);  // EncounterDefinition.cpp
ActorID GBAddWithEncounterAddStruct(ActorAddStruct & tActorAdd, EncounterAddStruct & tEncounterAdd);  // Actors.cpp
BOOL SPlayerFindGoodSpawnLocation(const WorldPlace & placeStart, WorldPlace & placeResult, float flSearchRadius);  // SPlayer.cpp
BOOL ScenesCommonNavMeshNodeFromPlace(const WorldPlace & tPlace, NavMeshHandle * ptNavMeshHandle, const BOOL fCanActivateScenes, const BOOL fProcessInactive);  // SceneCommon.cpp

// Act.h:13-19 in the DWARF, as Act.cpp and SPlayer.cpp repeat it.
struct CachedWaypoint {  // size 0x2c
    Act eSourceAct;  // 0x00
    WaypointInfo tInfo;  // 0x04
};
typedef blz::vector<CachedWaypoint, blz::allocator<CachedWaypoint> > CachedWaypointArray;
const CachedWaypointArray & ActGetCachedWaypointArray();  // Act.cpp

#ifndef D3_MOD_WORLD_BOSS_MINUTES
#define D3_MOD_WORLD_BOSS_MINUTES 10
#endif

namespace {

const float kMinDistance = 150.0f;  // world units from the hero to the spot: out of sight
const float kNearDistance = 70.0f;  // D3_WORLD_BOSS_NEAR: close, for testing
const float kWakeDistance = 90.0f;  // the boss appears when the hero is this close
const int kMaxScenes = 1024;
const int kMinLeaveSeconds = 30 * 60;  // an announced boss nobody comes for leaves after this

enum State
{
    STATE_WAITING,  // until gNext
    STATE_ANNOUNCED,  // the spot is marked; the boss appears when the hero gets there
    STATE_FIGHTING,  // the boss is out
};

State gState = STATE_WAITING;
time_t gNext = 0;
SNOName gBossName;
Team gBossTeam;
WorldPlace gSpot;
SSceneID gSpotScene = SSCENEID_INVALID;  // SSCENEID_INVALID until the hero enters the zone
SNO gZoneWorld = SNO_NONE;  // the announced zone: its world
SNO gZoneArea = SNO_NONE;  // and its level area
time_t gLeaves = 0;
ACDID gBoss = ACDID_INVALID;
char gAnnounced[256];
char gAppeared[256];
char gSlain[256];
char gNear[256];
char gLeft[256];

bool Trace()
{
    static bool bTrace = getenv("D3_WORLD_BOSS_TRACE") != NULL;
    return bTrace;
}

int IntervalSeconds()
{
    static int nSeconds = -1;
    if (nSeconds < 0)
    {
        const char * szEnv = getenv("D3_WORLD_BOSS_SECONDS");
        nSeconds = szEnv && atoi(szEnv) > 0 ? atoi(szEnv) : D3_MOD_WORLD_BOSS_MINUTES * 60;
    }
    return nSeconds;
}

float Distance(const WorldPlace & tA, const WorldPlace & tB)
{
    return sqrtf(Vector3D::Distance2DSquared(tA.wp, tB.wp));
}

void Announce(const char * szLabel)
{
    DisplayGameMessageForAllPlayers(szLabel, STRINGLIST_DEFAULT_INT_PARAM, STRINGLIST_DEFAULT_INT_PARAM);
    DisplayChatGameMessageForAllPlayers(szLabel, STRINGLIST_DEFAULT_INT_PARAM, STRINGLIST_DEFAULT_INT_PARAM);
}

// The spot's marker, as Minimap::AddQuestMarker sets up a bounty event's: hidden once the
// hero is within 60, an arrow at the minimap's edge when off it, not hidden by unexplored map.
void SendSpotMarker(bool bActive)
{
    MapMarkerInfoMessage tMsg;
    memset(&tMsg, 0, sizeof(tMsg));
    tMsg.m_nHashedName = MarkerStringHash("D3ModWorldBoss");
    tMsg.m_Place = gSpot;
    tMsg.m_ImageInfo = GlobalSNOGet(GLOBALSNO_TEXTURE_MARKER_EVENT_LOCATION);
    tMsg.m_snoStringList = SNO_NONE;
    tMsg.m_hLabel = (uint32)-1;
    tMsg.m_bActive = bActive;
    tMsg.m_bCanBecomeArrow = TRUE;
    tMsg.m_bRespectsFoW = FALSE;
    tMsg.m_fMinDisplayRangeSq = 3600.0f;
    tMsg.m_snoKnownActorOverride = SNO_NONE;
    tMsg.m_snoQuestSource = SNO_NONE;
    PlayerList listPlayers(PlayerListGetElementPool());
    PlayerListGetAllInGame(listPlayers, -1, FALSE, -1);
    MessageSendToClient(listPlayers, SMSG_MAP_MARKER, &tMsg, sizeof(tMsg));
}

// A random point in a random scene of the hero's world, far enough away. Scenes the game
// has not loaded are preferred, and nothing is loaded here. A point the navigation data
// already knows to be walkable is preferred; otherwise it is snapped on arrival.
bool PickSpot(const ActorCommonData & tACDHero, bool bNear)
{
    const WorldPlace & tHero = tACDHero.GetPlace();
    SceneCommonData * aptScenes[kMaxScenes];
    int nScenes = 0;
    int nLoaded = 0;
    SceneCommonDataIterator tIterator;
    SceneCommonData * ptSCD = NULL;
    while (SceneCommonDataArrayIterate(tIterator, ptSCD, SkipHolodeckScenes) && nScenes < kMaxScenes)
    {
        if (ptSCD->idSWorld == tHero.idSWorld && ptSCD->tSceneSpec.arSnoLevelAreas[0] != SNO_NONE &&
            (gZoneArea == SNO_NONE || ptSCD->tSceneSpec.arSnoLevelAreas[0] == gZoneArea))
        {
            aptScenes[nScenes++] = ptSCD;
            nLoaded += (ptSCD->dwSceneFlags & (1 << SCENE_ACTIVATED)) ? 1 : 0;
        }
    }
    if (Trace())
    {
        for (int i = 0; i < nScenes; i++)
        {
            fprintf(stderr, "worldboss: scene at (%.0f, %.0f) %s\n", aptScenes[i]->transformWorld.wp.x, aptScenes[i]->transformWorld.wp.y,
                    (aptScenes[i]->dwSceneFlags & (1 << SCENE_ACTIVATED)) ? "loaded" : "not loaded");
        }
    }
    const int nAllScenes = nScenes;
    bool bFound = false;
    bool bWalkable = false;
    float flX, flY;
    const char * szSpot = getenv("D3_WORLD_BOSS_SPOT");
    if (szSpot && sscanf(szSpot, "%f,%f", &flX, &flY) == 2)
    {
        for (int i = 0; i < nScenes; i++)
        {
            WorldPlace tPoint;
            tPoint.idSWorld = tHero.idSWorld;
            tPoint.wp = tHero.wp;
            tPoint.wp.x = flX;
            tPoint.wp.y = flY;
            SceneCommonData * ptScene = aptScenes[i];
            Vector3D wpMin = ptScene->transformWorld.wp + ptScene->aabbBoundsLocalLogical.wp - ptScene->aabbBoundsLocalLogical.wvExt;
            Vector3D wpMax = ptScene->transformWorld.wp + ptScene->aabbBoundsLocalLogical.wp + ptScene->aabbBoundsLocalLogical.wvExt;
            if (flX >= wpMin.x && flX < wpMax.x && flY >= wpMin.y && flY < wpMax.y)
            {
                NavMeshHandle tHandle;
                bWalkable = ScenesCommonNavMeshNodeFromPlace(tPoint, &tHandle, FALSE, TRUE) != FALSE;
                gSpot = tPoint;
                gSpotScene = ptScene->idSScene;
                bFound = true;
            }
        }
        if (bFound)
        {
            nScenes = 0;  // skip the random search
        }
    }
    for (int nTry = 0; nTry < 200 && nScenes > 0 && !bWalkable; nTry++)
    {
        SceneCommonData * ptScene = aptScenes[rand() % nScenes];
        bool bLoaded = (ptScene->dwSceneFlags & (1 << SCENE_ACTIVATED)) != 0;
        if (!bNear && bLoaded && nLoaded < nScenes && nTry < 150)
        {
            continue;  // somewhere not loaded yet, while there is such a place
        }
        const AABB & tBox = ptScene->aabbBoundsLocalLogical;
        WorldPlace tPoint;
        tPoint.idSWorld = tHero.idSWorld;
        tPoint.wp = ptScene->transformWorld.wp + tBox.wp;
        tPoint.wp.x += tBox.wvExt.x * (2.0f * rand() / RAND_MAX - 1.0f) * 0.9f;
        tPoint.wp.y += tBox.wvExt.y * (2.0f * rand() / RAND_MAX - 1.0f) * 0.9f;
        tPoint.wp.z = tHero.wp.z;  // the box's middle height is in the air
        float flDistance = Distance(tPoint, tHero);
        if (bNear ? (flDistance > kNearDistance || flDistance < 20.0f) : flDistance < kMinDistance)
        {
            continue;
        }
        NavMeshHandle tHandle;
        bWalkable = ScenesCommonNavMeshNodeFromPlace(tPoint, &tHandle, FALSE, TRUE) != FALSE;
        if (bWalkable || !bFound)
        {
            gSpot = tPoint;
            gSpotScene = ptScene->idSScene;
            bFound = true;
        }
    }
    if (bFound)
    {
        SceneCommonData * ptScene = SceneCommonDataTryToGet(gSpotScene);
        fprintf(stderr, "worldboss: spot at (%.0f, %.0f), %.0f from the hero, %s, %s (%d of %d scenes loaded)\n",
                gSpot.wp.x, gSpot.wp.y, Distance(gSpot, tHero),
                ptScene && (ptScene->dwSceneFlags & (1 << SCENE_ACTIVATED)) ? "loaded" : "not loaded",
                bWalkable ? "known walkable" : "to be snapped on arrival", nLoaded, nAllScenes);
    }
    return bFound;
}

// The zone: a waypoint of the hero's act that is not a town, and not the hero's own zone while
// there is another. The act is the one whose waypoints include the hero's world; failing that,
// any. Where the game keeps track (not adventure mode), only waypoints the hero has reached.
// D3_WORLD_BOSS_TEST picks the hero's own zone, a town too; D3_WORLD_BOSS_NEAR, when not a town.
bool PickZone(const Player & tPlayer, const ActorCommonData & tACDHero, bool bHere, bool bTown)
{
    SNO snoHeroWorld = WorldCommonGetWorldSNO(tACDHero.GetPlace().idSWorld);
    SNO snoHeroArea = ACDGetLevelArea(tACDHero);
    if (bHere)
    {
        if (snoHeroArea == SNO_NONE || (LevelAreaIsTown(snoHeroArea) && !bTown))
        {
            fprintf(stderr, "worldboss: the hero's own zone was asked for, but the hero is in town\n");
            return false;
        }
        gZoneWorld = snoHeroWorld;
        gZoneArea = snoHeroArea;
        return true;
    }
    const CachedWaypointArray & tWaypoints = ActGetCachedWaypointArray();
    const int nCount = (int)tWaypoints.size();
    Act eAct = -1;
    for (int i = 0; i < nCount; i++)
    {
        if (tWaypoints[i].tInfo.snoWorld == snoHeroWorld)
        {
            eAct = tWaypoints[i].eSourceAct;
            break;
        }
    }
    int anPicks[128];
    int nPicks = 0;
    for (int nPass = 0; nPass < 2 && nPicks == 0; nPass++)
    {
        for (int i = 0; i < nCount && nPicks < 128; i++)
        {
            const WaypointInfo & tInfo = tWaypoints[i].tInfo;
            if ((eAct >= 0 && tWaypoints[i].eSourceAct != eAct) || LevelAreaIsTown(tInfo.snoLevelArea) ||
                !PlayerHasActivatedWaypoint(tPlayer, i))
            {
                continue;
            }
            if (nPass == 0 && tInfo.snoLevelArea == snoHeroArea)
            {
                continue;  // somewhere else first
            }
            anPicks[nPicks++] = i;
        }
    }
    if (nPicks == 0)
    {
        fprintf(stderr, "worldboss: no waypoint zone to send the hero to (%d waypoints, act %d)\n", nCount, eAct);
        return false;
    }
    const WaypointInfo & tInfo = tWaypoints[anPicks[rand() % nPicks]].tInfo;
    gZoneWorld = tInfo.snoWorld;
    gZoneArea = tInfo.snoLevelArea;
    return true;
}

bool Announce(const Player & tPlayer, const ActorCommonData & tACDHero, bool bHere, bool bTown)
{
    if (!PickZone(tPlayer, tACDHero, bHere, bTown))
    {
        return false;
    }
    gSpotScene = SSCENEID_INVALID;
    RNG tRNG;
    RandSetSeed_Internal(tRNG, (uint32)rand());
    if (!EncounterResolveToActorSpawnInfo(GlobalSNOGet(GLOBALSNO_ENCOUNTER_DUNGEON_FINDER_BOSS), tRNG, gBossName, gBossTeam))
    {
        fprintf(stderr, "worldboss: the rift guardian encounter gave no boss\n");
        return false;
    }
    CRefString sBoss;
    if (!MonsterGetBaseName(gBossName, sBoss))
    {
        sBoss = MonsterGetSNOName(gBossName);
    }
    CRefString sArea = LevelAreaGetLocalizedName(gZoneArea);
    snprintf(gAnnounced, sizeof(gAnnounced), "A world boss, %s, waits in %s. Go there to find it!", sBoss.str(), sArea.str());
    snprintf(gNear, sizeof(gNear), "%s is somewhere in %s. Follow the marker on your map!", sBoss.str(), sArea.str());
    snprintf(gAppeared, sizeof(gAppeared), "%s has appeared!", sBoss.str());
    snprintf(gSlain, sizeof(gSlain), "The world boss %s has been slain!", sBoss.str());
    snprintf(gLeft, sizeof(gLeft), "The world boss %s has left %s.", sBoss.str(), sArea.str());
    fprintf(stderr, "worldboss: %s (world %d, level area %d)\n", gAnnounced, gZoneWorld, gZoneArea);
    Announce("General:D3Mod_WorldBossAnnounced");
    return true;
}

ACDID SpawnBoss()
{
    WorldPlace tPlace = gSpot;
    WorldPlace tFound;
    if (SPlayerFindGoodSpawnLocation(gSpot, tFound, 40.0f) || SPlayerFindGoodSpawnLocation(gSpot, tFound, 80.0f))
    {
        tPlace = tFound;
    }
    ActorAddStruct tActorAdd;
    tActorAdd.tSNOName = gBossName;
    tActorAdd.snoActor = gBossName.snoHandle;
    tActorAdd.tPlace = tPlace;
    tActorAdd.eMonsterRarity = MR_BOSS;
    tActorAdd.eTeam = gBossTeam;
    tActorAdd.dwActorAddFlags |= (1 << ACTORADD_EVEN_WHEN_STUCK);
    tActorAdd.dwActorAddFlags |= (1 << ACTORADD_DONT_PERTURB);
    ScenesCommonNavMeshNodeFromPlace(tActorAdd.tPlace, &tActorAdd.tNavMeshHandle, FALSE, FALSE);
    GBHandleList listGroupAffixes(GBGetHandlePool());
    EncounterAddStruct tEncounterAdd;
    EncounterAddInitialize(tEncounterAdd, listGroupAffixes);
    ActorID idActor = GBAddWithEncounterAddStruct(tActorAdd, tEncounterAdd);
    ACDID idBoss = idActor != ACTORID_INVALID ? ACDFindByActor(idActor) : ACDID_INVALID;
    if (!ACDTryToGet(idBoss))
    {
        fprintf(stderr, "worldboss: the game would not add the boss\n");
        return ACDID_INVALID;
    }

    // On the minimap, from anywhere in the area.
    RegisterQuestActor(idBoss, TRUE);
    PlayerList listPlayers(PlayerListGetElementPool());
    PlayerListGetAllInGame(listPlayers, -1, FALSE, -1);
    POS pos = listPlayers.GetHeadPosition();
    while (pos)
    {
        if (Player * ptPlayer = listPlayers.GetNext(pos))
        {
            SPlayerTrickleAdd(*ptPlayer, idBoss);
        }
    }
    fprintf(stderr, "worldboss: %s at (%.0f, %.0f), %.0f from the marked spot\n", gAppeared, tPlace.wp.x, tPlace.wp.y,
            Distance(tPlace, gSpot));
    Announce("General:D3Mod_WorldBossAppeared");
    return idBoss;
}

void Wait(time_t tNow, int nSeconds)
{
    gState = STATE_WAITING;
    gNext = tNow + nSeconds;
    gBoss = ACDID_INVALID;
}

}  // namespace

const char * D3ModWorldBossString(SNO, const char * szLabel)
{
    if (!szLabel || strncmp(szLabel, "D3Mod_WorldBoss", 15) != 0)
    {
        return NULL;
    }
    if (Trace())
    {
        fprintf(stderr, "worldboss: text for %s fetched\n", szLabel);
    }
    if (strcmp(szLabel, "D3Mod_WorldBossAnnounced") == 0)
    {
        return gAnnounced;
    }
    if (strcmp(szLabel, "D3Mod_WorldBossAppeared") == 0)
    {
        return gAppeared;
    }
    if (strcmp(szLabel, "D3Mod_WorldBossSlain") == 0)
    {
        return gSlain;
    }
    if (strcmp(szLabel, "D3Mod_WorldBossNear") == 0)
    {
        return gNear;
    }
    if (strcmp(szLabel, "D3Mod_WorldBossLeft") == 0)
    {
        return gLeft;
    }
    return NULL;
}

void D3ModWorldBossUpdate()
{
    static bool bTest = getenv("D3_WORLD_BOSS_TEST") != NULL;
    static bool bNear = getenv("D3_WORLD_BOSS_NEAR") != NULL;
    Player * ptPlayer = PlayerGetFirstInGame();
    ActorCommonData * ptHero = ptPlayer ? ACDTryToGet(ptPlayer->idPlayerACD) : NULL;
    time_t tNow = time(NULL);
    if (!ptHero)
    {
        gState = STATE_WAITING;
        gNext = 0;
        gBoss = ACDID_INVALID;
        return;
    }
    if (gNext == 0)
    {
        srand((unsigned)tNow);
        Wait(tNow, IntervalSeconds());
        fprintf(stderr, "worldboss: on, every %d s\n", IntervalSeconds());
    }
    const WorldPlace & tHero = ptHero->GetPlace();

    switch (gState)
    {
    case STATE_WAITING:
        if (tNow < gNext)
        {
            static time_t tLastTrace = 0;
            if (Trace() && tNow - tLastTrace >= 10)
            {
                fprintf(stderr, "worldboss: next in %d s\n", (int)(gNext - tNow));
                tLastTrace = tNow;
            }
            return;
        }
        // Not in a rift, whose guardian is its own event.
        {
            const char * szWait = NULL;
            if (ACDIsParticipatingInTieredLootRun(*ptHero))
                szWait = "the hero is in a greater rift";
            else if (WorldGetDungeonFinderLevel(WorldCommonGetWorldSNO(tHero.idSWorld)) >= 0)
                szWait = "the hero is in a rift";
            static const char * szLastWait = NULL;
            if (szWait != szLastWait)
            {
                if (szWait)
                    fprintf(stderr, "worldboss: due, but %s; trying again every 30 s\n", szWait);
                szLastWait = szWait;
            }
            if (szWait)
            {
                gNext = tNow + 30;
                return;
            }
        }
        if (Announce(*ptPlayer, *ptHero, bTest || bNear, bTest))
        {
            gState = STATE_ANNOUNCED;
            gLeaves = tNow + (IntervalSeconds() * 3 > kMinLeaveSeconds ? IntervalSeconds() * 3 : kMinLeaveSeconds);
        }
        else
        {
            gNext = tNow + 30;
        }
        return;

    case STATE_ANNOUNCED:
    {
        if (gSpotScene != SSCENEID_INVALID && tHero.idSWorld != gSpot.idSWorld)
        {
            fprintf(stderr, "worldboss: the hero left the zone before the boss came; it waits there still\n");
            gSpotScene = SSCENEID_INVALID;
        }
        if (tNow >= gLeaves)
        {
            fprintf(stderr, "worldboss: nobody came; %s\n", gLeft);
            if (gSpotScene != SSCENEID_INVALID)
            {
                SendSpotMarker(false);
            }
            Announce("General:D3Mod_WorldBossLeft");
            Wait(tNow, IntervalSeconds());
            return;
        }
        if (gSpotScene == SSCENEID_INVALID)
        {
            if (WorldCommonGetWorldSNO(tHero.idSWorld) != gZoneWorld || ACDIsParticipatingInTieredLootRun(*ptHero))
            {
                return;
            }
            if (!PickSpot(*ptHero, bNear))
            {
                if (Trace())
                {
                    fprintf(stderr, "worldboss: in the zone's world, but no scene of the zone yet\n");
                }
                return;
            }
            SendSpotMarker(true);
            Announce("General:D3Mod_WorldBossNear");
            return;
        }
        SceneCommonData * ptScene = SceneCommonDataTryToGet(gSpotScene);
        bool bLoaded = ptScene && (ptScene->dwSceneFlags & (1 << SCENE_ACTIVATED));
        if (Trace())
        {
            fprintf(stderr, "worldboss: hero at (%.0f, %.0f), %.0f from the spot, its scene %s\n", tHero.wp.x, tHero.wp.y,
                    Distance(tHero, gSpot), bLoaded ? "loaded" : "not loaded");
        }
        if (Distance(tHero, gSpot) > kWakeDistance || !bLoaded)
        {
            return;
        }
        gBoss = SpawnBoss();
        SendSpotMarker(false);
        if (gBoss != ACDID_INVALID)
        {
            gState = STATE_FIGHTING;
        }
        else
        {
            Wait(tNow, 30);
        }
        return;
    }

    case STATE_FIGHTING:
    {
        ActorCommonData * ptBoss = ACDTryToGet(gBoss);
        if (ptBoss && ptBoss->IsAlive() && ptBoss->GetPlace().idSWorld == tHero.idSWorld)
        {
            return;
        }
        if (ptBoss && !ptBoss->IsAlive())
        {
            fprintf(stderr, "worldboss: slain\n");
            Announce("General:D3Mod_WorldBossSlain");
        }
        else
        {
            fprintf(stderr, "worldboss: gone (the hero left its area)\n");
        }
        Wait(tNow, IntervalSeconds());
        return;
    }
    }
}
