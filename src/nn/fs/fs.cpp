// nn::fs: files and directories.
//
// The game names files as "<mount>:/path". Each mount is a folder on the host:
//   MountRom       -> <game>/romfs          (read-only game data)
//   MountSaveData  -> the save folder       (see SaveRoot below)
// Mount names the game invents map to whatever folder they were mounted on.
#include "nn/nn_types.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Result values the game tests for: module 2 is the file system.
nn::Result Fail(int description) { return nn::Result{2u | static_cast<uint32_t>(description) << 9}; }
nn::Result PathNotFound() { return Fail(1); }
nn::Result PathAlreadyExists() { return Fail(2); }
nn::Result NotMounted() { return Fail(6001); }
nn::Result Unexpected() { return Fail(5000); }

nn::Result FromErrno()
{
    switch (errno)
    {
    case ENOENT:
    case ENOTDIR:
        return PathNotFound();
    case EEXIST:
    case ENOTEMPTY:
        return PathAlreadyExists();
    default:
        return Unexpected();
    }
}

std::mutex g_mutex;

// Created on first use: the game may call in before this file's own start-up runs.
std::map<std::string, std::string> & Mounts()
{
    static std::map<std::string, std::string> * mounts = new std::map<std::string, std::string>;
    return *mounts;
}

std::string GameRoot()
{
    const char * env = getenv("D3_GAME_DIR");
    return env ? env : D3_DEFAULT_GAME_DIR;
}

std::string SaveRoot()
{
    const char * env = getenv("D3_SAVE_DIR");
    return env ? env : D3_DEFAULT_SAVE_DIR;
}

void MakeDirs(const std::string & path)
{
    for (size_t i = 1; i <= path.size(); i++)
    {
        if (i == path.size() || path[i] == '/')
        {
            mkdir(path.substr(0, i).c_str(), 0755);
        }
    }
}

// "data:/a/b" -> "<folder of data>/a/b". False if the mount name is unknown.
bool Resolve(const char * path, std::string * out)
{
    const char * colon = strchr(path, ':');
    if (!colon)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = Mounts().find(std::string(path, colon - path));
    if (it == Mounts().end())
    {
        return false;
    }
    const char * rest = colon + 1;
    while (*rest == '/')
    {
        rest++;
    }
    *out = it->second + "/" + rest;
    return true;
}

void Mount(const char * name, const std::string & folder)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Mounts()[name] = folder;
}

struct Directory {
    std::vector<std::pair<std::string, int64_t>> entries;   // name, size (-1 for a folder)
    size_t next = 0;
};

}  // namespace

// The save folder, for host code outside nn::fs (the Cheats mod keeps its files there).
extern "C" const char * d3_save_dir(void)
{
    static const std::string root = SaveRoot();
    return root.c_str();
}

// Mods mount extra host folders under names of their own (src/mods/loose_files.cpp: "d3loose").
extern "C" void d3_fs_mount(const char * name, const char * folder)
{
    Mount(name, folder);
}

namespace nn { namespace fs {

struct FileHandle { void * handle; };
struct DirectoryHandle { void * handle; };
struct WriteOption { int flags; };

struct DirectoryEntry {
    char name[769];
    char reserved1[3];
    int8_t directoryEntryType;      // 0 folder, 1 file
    char reserved2[3];
    int64_t fileSize;
};
static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry");

// ---- mounting ----

Result QueryMountRomCacheSize(unsigned long * size)
{
    *size = 0;
    return ResultSuccess();
}

Result MountRom(const char * name, void *, unsigned long)
{
    Mount(name, GameRoot() + "/romfs");
    return ResultSuccess();
}

// One save folder per user id.
static std::string SaveFolder(const account::Uid & user)
{
    char id[40];
    snprintf(id, sizeof id, "%016llx%016llx", static_cast<unsigned long long>(user.data[1]),
             static_cast<unsigned long long>(user.data[0]));
    return SaveRoot() + "/" + id;
}

Result EnsureSaveData(const account::Uid & user)
{
    MakeDirs(SaveFolder(user));
    return ResultSuccess();
}

bool IsSaveDataExisting(const account::Uid & user)
{
    struct stat st;
    return stat(SaveFolder(user).c_str(), &st) == 0;
}

Result MountSaveData(const char * name, const account::Uid & user)
{
    MakeDirs(SaveFolder(user));
    Mount(name, SaveFolder(user));
    return ResultSuccess();
}

// Writes go straight to the host's files, so there is nothing to commit.
Result Commit(const char *)
{
    return ResultSuccess();
}

void Unmount(const char * name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Mounts().erase(name);
}

// Add-on content is not supported: there is none to mount.
Result QueryMountAddOnContentCacheSize(unsigned long * size, int)
{
    *size = 0;
    return ResultSuccess();
}

Result MountAddOnContent(const char *, int, void *, unsigned long)
{
    return PathNotFound();
}

// ---- files ----

Result OpenFile(FileHandle * out, const char * path, int mode)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    int fd = open(host.c_str(), (mode & 2) ? O_RDWR : O_RDONLY);
    // D3_FS_TRACE=1 lists every file the game opens, and which ones were not there.
    static const bool trace = getenv("D3_FS_TRACE") != nullptr;
    if (trace)
    {
        fprintf(stderr, "fs: open %s%s\n", path, fd < 0 ? "  (not found)" : "");
    }
    if (fd < 0)
    {
        return FromErrno();
    }
    out->handle = reinterpret_cast<void *>(static_cast<intptr_t>(fd) + 1);
    return ResultSuccess();
}

static int Fd(FileHandle file)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(file.handle) - 1);
}

void CloseFile(FileHandle file)
{
    close(Fd(file));
}

Result GetFileSize(long * out, FileHandle file)
{
    struct stat st;
    if (fstat(Fd(file), &st) != 0)
    {
        return FromErrno();
    }
    *out = st.st_size;
    return ResultSuccess();
}

Result ReadFile(unsigned long * out, FileHandle file, long offset, void * buffer, unsigned long size)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pread(Fd(file), static_cast<char *>(buffer) + done, size - done, offset + done);
        if (n < 0)
        {
            return FromErrno();
        }
        if (n == 0)
        {
            break;
        }
        done += n;
    }
    *out = done;
    return ResultSuccess();
}

Result WriteFile(FileHandle file, long offset, const void * buffer, unsigned long size, const WriteOption &)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pwrite(Fd(file), static_cast<const char *>(buffer) + done, size - done, offset + done);
        if (n < 0)
        {
            return FromErrno();
        }
        done += n;
    }
    return ResultSuccess();
}

Result CreateFile(const char * path, long size)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    int fd = open(host.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
    {
        return FromErrno();
    }
    if (size > 0)
    {
        ftruncate(fd, size);
    }
    close(fd);
    return ResultSuccess();
}

Result DeleteFile(const char * path)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    return unlink(host.c_str()) == 0 ? ResultSuccess() : FromErrno();
}

Result RenameFile(const char * from, const char * to)
{
    std::string hostFrom, hostTo;
    if (!Resolve(from, &hostFrom) || !Resolve(to, &hostTo))
    {
        return NotMounted();
    }
    struct stat st;
    if (stat(hostTo.c_str(), &st) == 0)
    {
        return PathAlreadyExists();
    }
    return rename(hostFrom.c_str(), hostTo.c_str()) == 0 ? ResultSuccess() : FromErrno();
}

// ---- directories ----

Result CreateDirectory(const char * path)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    return mkdir(host.c_str(), 0755) == 0 ? ResultSuccess() : FromErrno();
}

Result DeleteDirectory(const char * path)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    return rmdir(host.c_str()) == 0 ? ResultSuccess() : FromErrno();
}

static bool RemoveTree(const std::string & folder)
{
    DIR * dir = opendir(folder.c_str());
    if (!dir)
    {
        return false;
    }
    while (dirent * e = readdir(dir))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        {
            continue;
        }
        std::string child = folder + "/" + e->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
        {
            RemoveTree(child);
        }
        else
        {
            unlink(child.c_str());
        }
    }
    closedir(dir);
    return rmdir(folder.c_str()) == 0;
}

Result DeleteDirectoryRecursively(const char * path)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    return RemoveTree(host) ? ResultSuccess() : FromErrno();
}

Result OpenDirectory(DirectoryHandle * out, const char * path, int mode)
{
    std::string host;
    if (!Resolve(path, &host))
    {
        return NotMounted();
    }
    DIR * dir = opendir(host.c_str());
    if (!dir)
    {
        return FromErrno();
    }
    Directory * d = new Directory;
    while (dirent * e = readdir(dir))
    {
        if (e->d_name[0] == '.')        // ".", ".." and host clutter such as .DS_Store
        {
            continue;
        }
        struct stat st;
        if (stat((host + "/" + e->d_name).c_str(), &st) != 0)
        {
            continue;
        }
        bool folder = S_ISDIR(st.st_mode);
        if ((folder && (mode & 1)) || (!folder && (mode & 2)))
        {
            d->entries.emplace_back(e->d_name, folder ? -1 : static_cast<int64_t>(st.st_size));
        }
    }
    closedir(dir);
    out->handle = d;
    return ResultSuccess();
}

void CloseDirectory(DirectoryHandle handle)
{
    delete static_cast<Directory *>(handle.handle);
}

Result GetDirectoryEntryCount(long * out, DirectoryHandle handle)
{
    *out = static_cast<long>(static_cast<Directory *>(handle.handle)->entries.size());
    return ResultSuccess();
}

Result ReadDirectory(long * outCount, DirectoryEntry * entries, DirectoryHandle handle, long count)
{
    Directory * d = static_cast<Directory *>(handle.handle);
    long n = 0;
    while (n < count && d->next < d->entries.size())
    {
        const auto & e = d->entries[d->next++];
        memset(&entries[n], 0, sizeof(DirectoryEntry));
        strncpy(entries[n].name, e.first.c_str(), sizeof(entries[n].name) - 1);
        entries[n].directoryEntryType = e.second < 0 ? 0 : 1;
        entries[n].fileSize = e.second < 0 ? 0 : e.second;
        n++;
    }
    *outCount = n;
    return ResultSuccess();
}

}}  // namespace nn::fs
