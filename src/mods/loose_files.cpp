// The Loose Files mod (D3_MOD_LOOSE_FILES): game data files read from a folder instead of
// the archives, so data made outside the game (tools/port10 converts the PvP arenas of the
// 1.0 PC release) can replace the game's own files or add new ones.
//
// The folder is <save folder>/loose, or D3_SNO_DIR when that is set. In it, a file is found
// by its group's folder name and its SNO number: Worlds/102100.wrl, Scene/100826.scn,
// Appearance/100823.app. Most groups keep only a hash of each file's name in the game's file
// list (CoreTOC.dat), so the number is what is matched; for the groups that keep names
// (sSNOGroupNeedsName in SNOFiles.cpp) <Group>/<name><ext> is tried as well.
//
// A file whose number is already in the game's list replaces the game's copy. A new number
// must also be named in the folder's index.txt, one file per line:
//
//     <group number> <SNO number> <name>
//
// for example "48 102100 PvP_Butcher_Arena_01". The name may hold spaces; lines starting
// with # are skipped. Those files are added to the game's list once it is read, as the
// game's own hotfix lists add theirs, so they can be found by number and by name (a world
// named in D3_LOAD_WORLD, say).
//
// D3_DUMP_TYPES=<file>, with the mod on, writes every type descriptor the game registers to
// <file> as JSON and quits once the file list is read. tools/port10 reads the layouts of
// 2.7.7's data files from it.
//
// D3_LOOSE_LOG=1 prints each file the game opens from the folder.
//
// The brawler in New Tristram (PVP_Dueling_NPC) sends heroes to the dueling world,
// PVP_Duel_Small. When the folder has ported 1.0 arenas, he sends them to those instead,
// taking each in turn from a random one: the arenas arenas.txt marks with 1 (tools/port10
// leaves the unfinished ones out), or, without arenas.txt, every world index.txt adds whose
// name starts with PvP_. D3_DUEL_ARENA=<world name> picks one, and D3_DUEL_ARENA=off keeps
// the game's own world. The arenas are built from PVP_Duel_Small (tools/port10/world.py),
// so they keep its dueling flag and its level area, whose script turns dueling on as a
// hero enters. That area is called Scorched Chapel; in an arena arenas.txt names, the
// area's name is the arena's instead (D3ModLooseString, through the shared string hook).
//
// The decomp's SNOFiles.cpp is patched to ask D3ModSnoOverride for every file it opens,
// SNOTOC.cpp to call D3ModLooseRegister once the game's file list is read, Prefetch.cpp
// to load folder files through D3ModSnoLoadLoose instead of queueing an archive read (which
// would find nothing), and to ask D3ModSnoIsLoose before it reports a file as not loaded,
// and ScriptUtil.cpp's PlayerWarpToWorld to ask D3ModDuelWorld where a warp goes.
#include "X/Common/Definitions/Definitions.h"
#include "X/Common/Game.h"
#include "X/Common/GlobalSNO.h"
#include "X/Common/SNO/SNOFiles.h"
#include "X/Sigma/Files/XFiles.h"
#include "X/Sigma/TypeDescriptor/TypeDescriptor.h"
#include "X/Sigma/TypeDescriptor/TypeDescriptorField.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" const char * d3_save_dir(void);                             // src/nn/fs/fs.cpp
extern "C" void d3_fs_mount(const char * name, const char * folder);  // src/nn/fs/fs.cpp
LPCSTR SNOGroupGetName(const SNOGroup snoGroup);                      // X/Common/SNO/SNOGroups.cpp
LPCSTR SNOGetGroupExtension(SNOGroup snoGroup);                       // X/Common/SNO/SNOGroups.cpp
void SNOFileAddEntry(SNOTOC::TOCType eType, const SNOGroup snoGroup, const uint32 uGuid, LPCSTR szFileNameNoExtension);  // X/Common/SNO/SNOFiles.cpp

// The loose folder, mounted for the game's file code as "d3loose:". Empty if it does not exist.
static const std::string & sLooseFolder()
{
    static std::string * spFolder;
    if (!spFolder)
    {
        const char * szEnv = getenv("D3_SNO_DIR");
        spFolder = new std::string(szEnv && *szEnv ? szEnv : std::string(d3_save_dir()) + "/loose");
        struct stat st;
        if (stat(spFolder->c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        {
            spFolder->clear();
        }
        else
        {
            d3_fs_mount("d3loose", spFolder->c_str());
        }
    }
    return *spFolder;
}

BOOL D3ModSnoOverride(int32 snoGroup, uint32 uGuid, LPCSTR szName, FileReference & tRef)
{
    const std::string & sFolder = sLooseFolder();
    if (sFolder.empty())
    {
        return FALSE;
    }
    char szFile[512];
    for (int i = 0; i < 2; ++i)
    {
        if (i == 0)
        {
            snprintf(szFile, sizeof szFile, "%s/%u%s", SNOGroupGetName(snoGroup), uGuid, SNOGetGroupExtension(snoGroup));
        }
        else if (szName && *szName)
        {
            snprintf(szFile, sizeof szFile, "%s/%s%s", SNOGroupGetName(snoGroup), szName, SNOGetGroupExtension(snoGroup));
        }
        else
        {
            break;
        }
        struct stat st;
        if (stat((sFolder + "/" + szFile).c_str(), &st) != 0)
        {
            continue;
        }
        char szPath[600];
        snprintf(szPath, sizeof szPath, "d3loose:/%s", szFile);
        static const char * szLog = getenv("D3_LOOSE_LOG");
        if (szLog && *szLog)
        {
            fprintf(stderr, "loose: %s\n", szFile);
        }
        FileReferenceInit(tRef, szPath);
        return TRUE;
    }
    return FALSE;
}

// What is known of each <Group>/<sno><ext>: whether it is in the folder, and whether the
// prefetcher has had it loaded. Asked from several threads and often.
struct LooseState
{
    bool bLoose;
    bool bTried;
};
static std::mutex sLooseMutex;
static std::unordered_map<uint64_t, LooseState> sLooseKnown;

static LooseState & sLooseStateLocked(int32 snoGroup, uint32 uGuid)
{
    uint64_t uKey = (static_cast<uint64_t>(static_cast<uint32>(snoGroup)) << 32) | uGuid;
    auto it = sLooseKnown.find(uKey);
    if (it != sLooseKnown.end())
    {
        return it->second;
    }
    const std::string & sFolder = sLooseFolder();
    bool bLoose = false;
    if (!sFolder.empty())
    {
        char szFile[512];
        snprintf(szFile, sizeof szFile, "%s/%s/%u%s", sFolder.c_str(), SNOGroupGetName(snoGroup), uGuid, SNOGetGroupExtension(snoGroup));
        struct stat st;
        bLoose = stat(szFile, &st) == 0;
    }
    return sLooseKnown[uKey] = LooseState{bLoose, false};
}

// The prefetcher reads only from the archives (X/Client/Prefetch.cpp). It is patched to call
// this first: a file in the folder is loaded here and then, on the calling thread, so the
// game finds it loaded as it would an archive file once its read completed. Returns TRUE
// when the file is in the folder (the prefetcher then leaves it alone).
BOOL D3ModSnoLoadLoose(int32 snoGroup, uint32 uGuid)
{
    {
        std::lock_guard<std::mutex> tLock(sLooseMutex);
        LooseState & tState = sLooseStateLocked(snoGroup, uGuid);
        if (!tState.bLoose)
        {
            return FALSE;
        }
        tState.bTried = true;
    }
    GroupDefinition * ptGroupDef = GameDefinitionGet((SNOGroup)snoGroup);
    if (ptGroupDef)
    {
        void * lpDef = const_cast<void *>(ptGroupDef->LockR((SNO)uGuid, TRUE));
        if (lpDef)
        {
            ptGroupDef->UnlockR(lpDef);
        }
    }
    return TRUE;
}

// Whether a file in the folder has been through D3ModSnoLoadLoose, for the prefetcher's
// status (also patched): one that would not load is then reported as loaded rather than
// unloaded, so whatever waits on it (a scene on its lightmap) does not wait forever.
BOOL D3ModSnoIsLoose(int32 snoGroup, uint32 uGuid)
{
    std::lock_guard<std::mutex> tLock(sLooseMutex);
    const LooseState & tState = sLooseStateLocked(snoGroup, uGuid);
    return tState.bLoose && tState.bTried;
}

// D3_DUMP_TYPES=<file>: the type descriptors as JSON, then quit.
static void sDumpTypes(const char * szOut)
{
    FILE * f = fopen(szOut, "w");
    if (!f)
    {
        fprintf(stderr, "loose: cannot write %s\n", szOut);
        exit(1);
    }
    fprintf(f, "{\n");
    bool bFirst = true;
    for (ComplexTypeDescriptor * t = ComplexTypeDescriptor::GetRegistrationHead(); t; t = t->GetNextRegisteredDescriptor())
    {
        fprintf(f, "%s\"%s@%p\": {\"name\": \"%s\", \"version\": %d, \"fields\": [", bFirst ? "" : ",\n",
                t->szClassName, (void *)t, t->szClassName, t->nVersion);
        bFirst = false;
        for (uint32 i = 0; i < t->nFieldCount; i++)
        {
            const TypeDescriptorField & fd = t->arFields[i];
            const uint32 * raw = (const uint32 *)&fd;
            fprintf(f, "%s{\"name\": %s%s%s, \"type\": \"%s\", \"offset\": %d, \"flags\": %u, \"aux\": \"%s\", \"ser\": %d, \"count\": %d, \"used\": %d, \"raw\": [",
                    i ? ", " : "", fd.szFieldName ? "\"" : "", fd.szFieldName ? fd.szFieldName : "null", fd.szFieldName ? "\"" : "",
                    fd.ptDataType ? fd.ptDataType->szClassName : "", fd.nDataOffset, fd.dwFlags,
                    fd.ptAuxDataType ? fd.ptAuxDataType->szClassName : "", fd.nSerOffset, fd.nCount, fd.nUsedElementsOffset);
            for (int k = 0; k < 48; k++)
            {
                fprintf(f, "%s%u", k ? "," : "", raw[k]);
            }
            fprintf(f, "]}");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    fprintf(stderr, "loose: type descriptors written to %s\n", szOut);
    exit(0);
}

SNO CGameGetCurrentWorld();  // X/Client/CGame.cpp

// The ported arenas, from index.txt and arenas.txt. Filled once, while the game reads its
// file list, before any game starts.
struct LooseArena
{
    std::string sWorld;
    uint32 uGuid;
    bool bRotation;     // Nek sends heroes here
    std::string sName;  // what the dueling world's area is called here, or empty
};
static std::vector<LooseArena> sArenaList;
static std::vector<std::string> sArenas;  // Nek's rotation

// The area name of the dueling world's level area (DuelWorld) in a ported arena: the
// arena's own. Every other string is the game's (NULL).
const char * D3ModLooseString(SNO snoStringList, const char * szLabel)
{
    if (!szLabel || strcmp(szLabel, "DuelWorld") != 0 || snoStringList != GlobalSNOGet(GLOBALSNO_STRINGLIST_LEVEL_AREAS))
    {
        return NULL;
    }
    SNO snoWorld = CGameGetCurrentWorld();
    for (const LooseArena & a : sArenaList)
    {
        if ((SNO)a.uGuid == snoWorld && !a.sName.empty())
        {
            return a.sName.c_str();
        }
    }
    return NULL;
}

// PlayerWarpToWorld's world name, or the arena to go to instead when it is the dueling world.
LPCSTR D3ModDuelWorld(LPCSTR szWorldName)
{
    if (!szWorldName || strcasecmp(szWorldName, "PVP_Duel_Small") != 0)
    {
        return szWorldName;
    }
    const char * szChoice = getenv("D3_DUEL_ARENA");
    if (szChoice && strcasecmp(szChoice, "off") == 0)
    {
        return szWorldName;
    }
    LPCSTR szArena = szWorldName;
    if (szChoice && *szChoice)
    {
        szArena = szChoice;
    }
    else if (!sArenas.empty())
    {
        static std::atomic<unsigned> snNext{std::random_device{}()};
        szArena = sArenas[snNext++ % sArenas.size()].c_str();
    }
    if (szArena != szWorldName)
    {
        fprintf(stderr, "loose: the dueling world is %s this time\n", szArena);
    }
    return szArena;
}

// Once the game's file list is read: add the files index.txt names. Returns bRead unchanged.
BOOL D3ModLooseRegister(BOOL bRead)
{
    const char * szDump = getenv("D3_DUMP_TYPES");
    if (szDump && *szDump)
    {
        sDumpTypes(szDump);
    }
    const std::string & sFolder = sLooseFolder();
    if (sFolder.empty())
    {
        return bRead;
    }
    FILE * f = fopen((sFolder + "/index.txt").c_str(), "r");
    int nAdded = 0;
    if (f)
    {
        char szLine[512];
        while (fgets(szLine, sizeof szLine, f))
        {
            szLine[strcspn(szLine, "\r\n")] = 0;
            int nGroup = 0;
            unsigned int uGuid = 0;
            int nNameAt = 0;
            if (szLine[0] == '#' || sscanf(szLine, "%d %u %n", &nGroup, &uGuid, &nNameAt) < 2 || !szLine[nNameAt])
            {
                continue;
            }
            SNOFileAddEntry(SNOTOC::TT_CORE, (SNOGroup)nGroup, uGuid, szLine + nNameAt);
            nAdded++;
            if (nGroup == WORLD_SNO && strncasecmp(szLine + nNameAt, "PvP_", 4) == 0)
            {
                sArenaList.push_back({szLine + nNameAt, uGuid, true, ""});
            }
        }
        fclose(f);
    }
    // arenas.txt: <world>\t<1 if Nek sends heroes there>\t<name>, one arena per line.
    f = fopen((sFolder + "/arenas.txt").c_str(), "r");
    if (f)
    {
        for (LooseArena & a : sArenaList)
        {
            a.bRotation = false;
        }
        char szLine[512];
        while (fgets(szLine, sizeof szLine, f))
        {
            szLine[strcspn(szLine, "\r\n")] = 0;
            char * szTab = strchr(szLine, '\t');
            char * szName = szTab ? strchr(szTab + 1, '\t') : NULL;
            if (szLine[0] == '#' || !szName)
            {
                continue;
            }
            *szTab = 0;
            for (LooseArena & a : sArenaList)
            {
                if (strcasecmp(a.sWorld.c_str(), szLine) == 0)
                {
                    a.bRotation = szTab[1] == '1';
                    a.sName = szName + 1;
                }
            }
        }
        fclose(f);
    }
    for (const LooseArena & a : sArenaList)
    {
        if (a.bRotation)
        {
            sArenas.push_back(a.sWorld);
        }
    }
    if (!sArenaList.empty())
    {
        fprintf(stderr, "loose: %zu arenas, %zu in Nek's rotation\n", sArenaList.size(), sArenas.size());
    }
    fprintf(stderr, "loose: files from %s (%d added by index.txt)\n", sFolder.c_str(), nAdded);
    return bRead;
}
