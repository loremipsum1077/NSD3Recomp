// D3_PVP_LOG=1: every five seconds of a PvP game type (D3_GAME_TYPE), the match's state on
// stderr: its phase, the round's clock, the team scores, and each hero's team and life.
// Appended to the decomp's X/Server/PVP.cpp, whose PVPUpdate calls it (DecompSources.cmake).
void D3ModPvpTrace()
{
    static int nOn = -1;
    if (nOn < 0)
    {
        const char * szLog = getenv("D3_PVP_LOG");
        nOn = szLog && *szLog ? 1 : 0;
    }
    static uint32 dwLast;
    uint32 dwNow = GameGetServerTime();
    if (!nOn || dwNow - dwLast < 5 * TICKS_PER_SECOND)
    {
        return;
    }
    dwLast = dwNow;
    static const char * const kStates[] = {"unstarted", "pregame", "game", "game over"};
    int nState = (int)GameGetPVPGameState();
    const GameSyncedData & tData = GameGetActiveGameCommonData().tGameSyncedData;
    int nLeft = tData.dwRoundEnd > dwNow ? (int)((tData.dwRoundEnd - dwNow) / TICKS_PER_SECOND) : 0;
    fprintf(stderr, "pvp: %s, round %s, %d s left, score %u-%u%s\n", nState >= 0 && nState < 4 ? kStates[nState] : "?",
            tData.dwRoundStart ? "started" : "not started", nLeft, tData.nTeamScore[0], tData.nTeamScore[1],
            tData.bPVPGameOver ? ", over" : "");
    for (Player * ptPlayer = PlayerGetFirstInGame(); ptPlayer; ptPlayer = PlayerGetNextInGame(ptPlayer))
    {
        ActorCommonData * ptACD = ACDTryToGet(ptPlayer->idPlayerACD);
        if (ptACD)
        {
            fprintf(stderr, "pvp:   team %d (actor %d), life %.0f of %.0f%s%s%s%s%s, pvp reduction %.2f\n", (int)ptPlayer->eTeam,
                    (int)ptACD->GetTeam(), ptACD->GetHitPointsCurrent(), ptACD->GetHitPointsMaxTotal(),
                    ptACD->IsAlive() ? "" : ", dead", ptACD->IsDueling() ? ", dueling" : "",
                    ptACD->AttributesGetInt(ATTRIBUTE_INVULNERABLE) ? ", invulnerable" : "",
                    ptACD->AttributesGetInt(ATTRIBUTE_UNTARGETABLE) ? ", untargetable" : "",
                    ptACD->AttributesGetInt(ATTRIBUTE_GHOSTED) ? ", ghosted" : "",
                    ptACD->AttributesGetFloat(FastAttribKey(ATTRIBUTE_CLASS_DAMAGE_REDUCTION_PERCENT_PVP)));
        }
    }
}
