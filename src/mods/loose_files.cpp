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
// The decomp's SNOFiles.cpp is patched to ask D3ModSnoOverride for every file it opens,
// SNOTOC.cpp to call D3ModLooseRegister once the game's file list is read, and Prefetch.cpp
// to ask D3ModSnoIsLoose before it reports a file as not loaded.
#include "X/Common/SNO/SNOFiles.h"
#include "X/Sigma/Files/XFiles.h"
#include "X/Sigma/TypeDescriptor/TypeDescriptor.h"
#include "X/Sigma/TypeDescriptor/TypeDescriptorField.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mutex>
#include <string>
#include <unordered_map>

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

// Whether <Group>/<sno><ext> is in the folder, for the prefetcher (X/Client/Prefetch.cpp is
// patched to ask before it reports a file as not loaded). Asked from several threads and
// often, so each answer is kept.
BOOL D3ModSnoIsLoose(int32 snoGroup, uint32 uGuid)
{
    const std::string & sFolder = sLooseFolder();
    if (sFolder.empty())
    {
        return FALSE;
    }
    static std::mutex sMutex;
    static std::unordered_map<uint64_t, bool> sKnown;
    uint64_t uKey = (static_cast<uint64_t>(static_cast<uint32>(snoGroup)) << 32) | uGuid;
    std::lock_guard<std::mutex> tLock(sMutex);
    auto it = sKnown.find(uKey);
    if (it != sKnown.end())
    {
        return it->second;
    }
    char szFile[512];
    snprintf(szFile, sizeof szFile, "%s/%s/%u%s", sFolder.c_str(), SNOGroupGetName(snoGroup), uGuid, SNOGetGroupExtension(snoGroup));
    struct stat st;
    bool bLoose = stat(szFile, &st) == 0;
    sKnown[uKey] = bLoose;
    return bLoose;
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
        }
        fclose(f);
    }
    fprintf(stderr, "loose: files from %s (%d added by index.txt)\n", sFolder.c_str(), nAdded);
    return bRead;
}
