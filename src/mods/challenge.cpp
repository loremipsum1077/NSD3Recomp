// Challenge rifts offline (D3_MOD_CHALLENGE). On the Switch the game fetched two files
// from Blizzard's publisher storage when the Challenge tab of the game settings opened
// (Console::Online::GetChallengeRiftData, X/Client/Console/LobbyService.cpp):
//
//   challengerift_config.dat  a D3::ChallengeRifts::ChallengeData: the challenge number,
//                             its start and end times and the Nephalem orb multiplier;
//   challengerift_<n>.dat     a D3::Leaderboard::WeeklyChallengeData: the rift (tier, seed,
//                             boss, floors, pylons), and the hero and account to play it with.
//
// Offline nothing arrives, so GetChallengeRiftData is patched to call this instead, which
// reads the files from a folder: D3_CHALLENGE_DIR, else D3_MOD_CHALLENGE_DIR (CMake's
// default is <game>/rift_data). They are the files the game downloaded, saved as they came;
// no challenge data ships with this project.
//
// Which challenge: D3_MOD_CHALLENGE_PICK is "rotate" or a file number. With "rotate" the
// distinct challenges in the folder (identical files counted once) take turns, moving on
// each time one is completed. The challenge number is always one more than the last one the
// account was rewarded for: the game gives the reward only for a higher number than that.
// The times are set so the challenge has started and ends a week from whenever it is asked
// for, so it never runs out.
#include "X/Client/Console/Online.h"
#include "X/Client/Console/GamerProfile.h"
#include "X/Common/OnlineService/Callback/Callback_Impl.h"
#include "X/Common/Protobuf/Account.pb.h"
#include "X/Common/Protobuf/ChallengeRifts.pb.h"
#include "X/Common/Protobuf/Leaderboard.pb.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef D3_MOD_CHALLENGE_DIR
#define D3_MOD_CHALLENGE_DIR "rift_data"
#endif
#ifndef D3_MOD_CHALLENGE_PICK
#define D3_MOD_CHALLENGE_PICK "rotate"
#endif

namespace {

bool sReadFile(const std::string & sPath, std::string & sData)
{
    FILE * file = fopen(sPath.c_str(), "rb");
    if (!file)
    {
        return false;
    }
    sData.clear();
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        sData.append(buffer, n);
    }
    fclose(file);
    return true;
}

// The challenge files in the folder, in file-number order, identical ones once.
std::vector<std::string> sChallengeFiles(const std::string & sDir)
{
    std::vector<std::pair<int, std::string> > arNumbered;
    if (DIR * dir = opendir(sDir.c_str()))
    {
        while (struct dirent * entry = readdir(dir))
        {
            int nNumber;
            char szTail[8];
            if (sscanf(entry->d_name, "challengerift_%d.%4s", &nNumber, szTail) == 2 && strcmp(szTail, "dat") == 0)
            {
                arNumbered.push_back(std::make_pair(nNumber, std::string(entry->d_name)));
            }
        }
        closedir(dir);
    }
    std::sort(arNumbered.begin(), arNumbered.end());
    std::vector<std::string> arFiles, arSeen;
    for (const auto & tEntry : arNumbered)
    {
        std::string sData;
        if (!sReadFile(sDir + "/" + tEntry.second, sData) ||
            std::find(arSeen.begin(), arSeen.end(), sData) != arSeen.end())
        {
            continue;
        }
        arSeen.push_back(sData);
        arFiles.push_back(tEntry.second);
    }
    return arFiles;
}

}  // namespace

namespace Console { namespace Online {

void D3ModChallengeGetData(int32 nUserIndex, OnlineService::Callback<void (StorageResult, const D3::ChallengeRifts::ChallengeData &, const D3::Leaderboard::WeeklyChallengeData &)> pfnCallback)
{
    const char * szDir = getenv("D3_CHALLENGE_DIR");
    std::string sDir = szDir ? szDir : D3_MOD_CHALLENGE_DIR;

    // The number to play under: one past the last challenge the account was rewarded for.
    uint32 uLastReward = 0;
    if (GamerProfile::IsActive(nUserIndex))
    {
        uLastReward = GamerProfile::GetAccount(nUserIndex).digest().challenge_rift_account_data().last_challenge_reward_earned();
    }
    uint32 uNumber = uLastReward + 1;

    std::string sFile;
    if (strcmp(D3_MOD_CHALLENGE_PICK, "rotate") == 0)
    {
        std::vector<std::string> arFiles = sChallengeFiles(sDir);
        if (!arFiles.empty())
        {
            sFile = arFiles[(uNumber - 1) % arFiles.size()];
        }
    }
    else
    {
        char szName[64];
        snprintf(szName, sizeof(szName), "challengerift_%02d.dat", atoi(D3_MOD_CHALLENGE_PICK));
        sFile = szName;
    }

    std::string sData;
    D3::Leaderboard::WeeklyChallengeData tData;
    if (sFile.empty() || !sReadFile(sDir + "/" + sFile, sData))
    {
        fprintf(stderr, "challenge: no challenge rift files in %s\n", sDir.c_str());
        pfnCallback(STORAGE_FAILED_FILE_NOT_FOUND, D3::ChallengeRifts::ChallengeData(), D3::Leaderboard::WeeklyChallengeData());
        return;
    }
    if (sData.empty() || !tData.ParsePartialFromArray(sData.data(), (int)sData.size()) ||
        !(tData.has_rift_snapshot() && tData.has_hero_snapshot() && tData.has_account_snapshot()))
    {
        fprintf(stderr, "challenge: %s/%s is not challenge rift data\n", sDir.c_str(), sFile.c_str());
        pfnCallback(STORAGE_FAILED_DATA_INVALID, D3::ChallengeRifts::ChallengeData(), D3::Leaderboard::WeeklyChallengeData());
        return;
    }
    // As GetChallengeRiftData does for files from before completion_milliseconds existed.
    if (!tData.rift_snapshot().has_completion_milliseconds())
    {
        tData.mutable_rift_snapshot()->set_completion_milliseconds(tData.rift_snapshot().deprecated_completion_seconds() * 1000);
    }

    // The config file, if there is one, gives the orb multiplier and hash; the number and
    // times are always this mod's.
    D3::ChallengeRifts::ChallengeData tConfig;
    std::string sConfig;
    if (sReadFile(sDir + "/challengerift_config.dat", sConfig))
    {
        tConfig.ParsePartialFromArray(sConfig.data(), (int)sConfig.size());
    }
    tConfig.set_challenge_number(uNumber);
    tConfig.set_challenge_start_unix_time(0);
    tConfig.set_challenge_last_broadcast_unix_time(0);
    // A week from now, as a real challenge would show; the game asks again every minute.
    tConfig.set_challenge_end_unix_time_console((int64)time(NULL) + 7 * 24 * 60 * 60);

    fprintf(stderr, "challenge: challenge %u is %s (tier %u)\n", uNumber, sFile.c_str(), tData.rift_snapshot().rift_tier());
    pfnCallback(STORAGE_SUCCESS, tConfig, tData);
}

} }
