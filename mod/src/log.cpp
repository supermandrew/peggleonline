#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

extern HMODULE g_module;

static FILE* g_log;
static std::mutex g_log_mutex;

const std::string& ModDir()
{
    static std::string dir = [] {
        char path[MAX_PATH];
        GetModuleFileNameA(g_module, path, MAX_PATH);
        std::string s(path);
        return s.substr(0, s.find_last_of("\\/") + 1);
    }();
    return dir;
}

// Each start writes a new log (and debug mode adds allocation records): keep only the newest few of each kind.
static void PruneOldFiles(const std::string& dir, const char* pattern, size_t keep)
{
    struct Entry {
        FILETIME written;
        std::string name;
    };
    std::vector<Entry> files;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
        files.push_back({fd.ftLastWriteTime, fd.cFileName});
    while (FindNextFileA(h, &fd));
    FindClose(h);
    if (files.size() <= keep)
        return;
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return CompareFileTime(&a.written, &b.written) > 0; });
    for (size_t i = keep; i < files.size(); ++i)
        DeleteFileA((dir + files[i].name).c_str());
}

void LogInit(const char* tag)
{
    std::string dir = ModDir() + "mods\\logs\\";
    CreateDirectoryA((ModDir() + "mods").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    PruneOldFiles(dir, "netplay-*.log", 20);
    PruneOldFiles(dir, "allocs-*.txt", 4);
    PruneOldFiles(dir, "frees-*.txt", 4);
    char name[64];
    // One file per running copy, so two copies on one PC don't interleave.
    snprintf(name, sizeof name, "netplay-%s.log", tag);
    g_log = _fsopen((dir + name).c_str(), "w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
