/* NVIDIA's own shader caches on Windows: the game's in a folder of its own, kept to a size, and the driver's
 * "Shader Cache Size" said in the log.
 *
 * A player's NVIDIA shader cache reached 40 GB (the setting allowed 16) and the game ran badly. Measured on
 * an RTX 5000 Ada (driver 32.0.15.9716) with one debug match per start, the driver's folders before and after:
 *   - The driver keeps a set of files for each program PATH: %LOCALAPPDATA%\NVIDIA\DXCache\*<key>.nvph for
 *     Direct3D (one 16 MB file after the first start, 256-512 MB after long play: 57 such on a test PC), and
 *     GLCache\<..>\<..>\<key>.bin/.toc for Vulkan (6-12 MB after a start, 200-900 MB after long play). The
 *     same exe started again adds nothing (Direct3D) or a little (Vulkan +7-10 MB); the same exe re-dated in
 *     place (an update) keeps its set; the same bytes in another folder make a whole new set - each test
 *     build unpacked somewhere else is a new 0.25-1 GB, and the old ones stay until the driver's limit, past
 *     which it throws shaders away and compiles them again during play.
 *   - A new path's first start costs time as well: the first frame waited 2.8-3.3 s for Direct3D's prewarm
 *     (0.6 s on a known path) and 3.3 s for Vulkan's warm start (0.1 s) - our own pipeline cache file does
 *     not spare it (577 MB loaded, the driver still compiled).
 * NVIDIA's Vulkan driver takes its cache's folder from __GL_SHADER_DISK_CACHE_PATH, read as it loads (also on
 * Windows: a start with it set wrote its set there and nothing in GLCache). So the Vulkan renderer puts the
 * driver's cache beside the game's own shader cache (<shader cache>\nvidia): it goes with the game's folder,
 * and the sets of old builds are trimmed here (oldest first, SSBU_NV_CACHE_MB, 3 GB by default) instead of
 * filling the driver's shared limit. Direct3D's DXCache has no such switch: the launcher's build\ssbu.exe
 * keeps one path, so updates reuse its set; the log says how full the driver's caches are against its
 * setting (NvAPI's driver settings, read only) and what to change when they are over it. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_nvcache.h"

void nv_cache_setting_words(int found, uint32_t mb, char *out, size_t size) {
    if (!found || !mb) snprintf(out, size, "Driver Default");
    else if (mb == 0xFFFFFFFFu) snprintf(out, size, "Unlimited");
    else if (mb >= 1024 && mb % 1024 == 0) snprintf(out, size, "%u GB", mb / 1024);
    else snprintf(out, size, "%u MB", mb);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct NvFile { char *path; size_t stem; uint64_t size, time; } NvFile;
typedef struct NvFiles { NvFile *v; uint32_t n, cap; uint64_t total; } NvFiles;

static uint64_t file_time(FILETIME t) { return (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime; }

/* every file under dir (recursive) */
static void list_files(const char *dir, NvFiles *out, int depth) {
    char pattern[MAX_PATH + 8];
    if (depth > 6 || snprintf(pattern, sizeof pattern, "%s\\*", dir) >= (int)sizeof pattern) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char path[MAX_PATH + 8];
        if (snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName) >= (int)sizeof path) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* (no link leads out of the folder) */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { list_files(path, out, depth + 1); continue; }
        uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        out->total += size;
        if (out->n == out->cap) {
            uint32_t cap = out->cap ? out->cap * 2 : 64;
            NvFile *more = realloc(out->v, sizeof *more * cap);
            if (!more) continue;
            out->v = more;
            out->cap = cap;
        }
        NvFile *f = &out->v[out->n];
        if (!(f->path = _strdup(path))) continue;
        const char *name = strrchr(f->path, '\\') + 1, *dot = strchr(name, '.');
        f->stem = dot ? (size_t)(dot - f->path) : strlen(f->path);
        f->size = size;
        f->time = file_time(fd.ftLastWriteTime);
        out->n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void free_files(NvFiles *f) {
    for (uint32_t i = 0; i < f->n; i++) free(f->v[i].path);
    free(f->v);
    memset(f, 0, sizeof *f);
}

/* the bytes of every file under dir */
static uint64_t dir_bytes(const char *dir) {
    NvFiles f = {0};
    list_files(dir, &f, 0);
    uint64_t total = f.total;
    free_files(&f);
    return total;
}

static int same_set(const NvFile *a, const NvFile *b) { return a->stem == b->stem && !_strnicmp(a->path, b->path, a->stem); }

uint64_t nv_cache_trim(const char *dir, uint64_t cap, uint64_t keep_ms, uint32_t *removed, uint64_t *removed_bytes) {
    NvFiles f = {0};
    list_files(dir, &f, 0);
    uint32_t gone = 0;
    uint64_t gone_bytes = 0, total = f.total;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    uint64_t now = file_time(now_ft), keep = keep_ms * 10000;
    while (total > cap) {
        /* the set whose newest file is the oldest (a set: the files of one name, .bin and .toc) */
        int oldest = -1;
        uint64_t oldest_time = 0;
        for (uint32_t i = 0; i < f.n; i++) {
            if (!f.v[i].path) continue;
            uint64_t t = f.v[i].time;
            for (uint32_t j = 0; j < f.n; j++)
                if (j != i && f.v[j].path && same_set(&f.v[i], &f.v[j]) && f.v[j].time > t) t = f.v[j].time;
            if (now - t < keep || now < t) continue;
            if (oldest < 0 || t < oldest_time) { oldest = (int)i; oldest_time = t; }
        }
        if (oldest < 0) break;   /* (only sets in use now are left) */
        NvFile set = f.v[oldest];
        char *stem = _strdup(set.path);
        if (!stem) break;
        int any = 0;
        for (uint32_t j = 0; j < f.n; j++) {
            if (!f.v[j].path || f.v[j].stem != set.stem || _strnicmp(f.v[j].path, stem, set.stem)) continue;
            if (DeleteFileA(f.v[j].path)) { total -= f.v[j].size; gone_bytes += f.v[j].size; any = 1; }
            free(f.v[j].path);
            f.v[j].path = NULL;   /* (one that could not go - open in a game - is not tried again) */
        }
        free(stem);
        gone += (uint32_t)any;
    }
    free_files(&f);
    if (removed) *removed = gone;
    if (removed_bytes) *removed_bytes = gone_bytes;
    return total;
}

/* The variable for this process and for any C runtime already loaded that keeps a copy of the environment
 * (the driver may read it through one) */
static void set_env(const char *name, const char *value) {
    SetEnvironmentVariableA(name, value);
    const char *crts[] = {"ucrtbase.dll", "msvcrt.dll"};
    for (size_t i = 0; i < sizeof crts / sizeof *crts; i++) {
        HMODULE m = GetModuleHandleA(crts[i]);
        typedef int (__cdecl *PutEnvS)(const char *, const char *);
        PutEnvS put = m ? (PutEnvS)(void *)GetProcAddress(m, "_putenv_s") : NULL;
        if (put) put(name, value);
    }
    _putenv_s(name, value);   /* (this program's own) */
}

#define NV_CACHE_KEEP_MS (10 * 60 * 1000)   /* (a set written in the last 10 minutes is a game's running now) */
static char vk_dir[MAX_PATH];

void nv_cache_vulkan_setup(const char *shader_cache_dir) {
    const char *on = getenv("SSBU_NV_CACHE");
    if (on && !strcmp(on, "0")) {
        fprintf(stderr, "[vulkan] NVIDIA's shader cache: left in %%LOCALAPPDATA%%\\NVIDIA\\GLCache (SSBU_NV_CACHE=0)\n");
        return;
    }
    char have[8];
    if (GetEnvironmentVariableA("__GL_SHADER_DISK_CACHE_PATH", have, sizeof have) ||
        GetEnvironmentVariableA("__GL_SHADER_DISK_CACHE", have, sizeof have))
        return;   /* (the player's own choice) */
    char dir[MAX_PATH];
    DWORD n = GetFullPathNameA(shader_cache_dir, sizeof dir - 8, dir, NULL);
    if (!n || n >= sizeof dir - 8) return;
    strcat(dir, "\\nvidia");
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    uint64_t cap = 3072ull << 20;
    const char *mb = getenv("SSBU_NV_CACHE_MB");
    if (mb && *mb) cap = strtoull(mb, NULL, 10) << 20;
    uint32_t removed = 0;
    uint64_t removed_bytes = 0, left = cap ? nv_cache_trim(dir, cap, NV_CACHE_KEEP_MS, &removed, &removed_bytes) : dir_bytes(dir);
    set_env("__GL_SHADER_DISK_CACHE_PATH", dir);
    snprintf(vk_dir, sizeof vk_dir, "%s", dir);
    char most[32];
    if (cap) snprintf(most, sizeof most, "%.0f MB", cap / 1048576.0);
    else snprintf(most, sizeof most, "no limit");
    fprintf(stderr, "[vulkan] NVIDIA's shader cache: the game's own folder %s (%.0f MB; at most %s, SSBU_NV_CACHE_MB)\n", dir,
            left / 1048576.0, most);
    if (removed)
        fprintf(stderr, "[vulkan] NVIDIA's shader cache: %u old sets removed (%.0f MB): other builds' or folders' (the driver keeps one per program path)\n",
                removed, removed_bytes / 1048576.0);
}

/* ---- the driver's setting: NvAPI's driver settings (DRS), read only ----------------------------------------- */
/* (nvapi64.dll exports one function, nvapi_QueryInterface; the others are asked for by their ids. NVDRS_SETTING
 * version 1: a version word, the name (2048 UTF-16), id, type, location, two flags, then the predefined and the
 * current value, each a 4,100-byte union whose first word is a DWORD value.) */
#define NVDRS_SETTING_SIZE 12320
#define NVDRS_CURRENT_VALUE 8220
#define SETTING_CACHE_SIZE 0x00AC8497u   /* "Shader disk cache maximum size", megabytes */
#define SETTING_CACHE_ON 0x00198FFFu     /* the shader cache on (1) or off (0) */

typedef void *(__cdecl *NvQuery)(unsigned id);
static int nv_setting(NvQuery q, void *session, void *profile, unsigned id, uint32_t *value) {
    typedef int (__cdecl *GetSetting)(void *, void *, unsigned, void *);
    GetSetting get = (GetSetting)q(0x73BF8338);
    unsigned char *buf = calloc(1, NVDRS_SETTING_SIZE);
    if (!get || !buf) { free(buf); return 0; }
    *(uint32_t *)buf = NVDRS_SETTING_SIZE | 1u << 16;
    int ok = get(session, profile, id, buf) == 0;
    if (ok) memcpy(value, buf + NVDRS_CURRENT_VALUE, sizeof *value);
    free(buf);
    return ok;
}

/* 1: read (found: the size is set in the global profile; size in MB; off: the cache is turned off) */
static int nv_read_settings(int *found, uint32_t *size, int *off) {
    HMODULE m = LoadLibraryExA("nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    NvQuery q = m ? (NvQuery)(void *)GetProcAddress(m, "nvapi_QueryInterface") : NULL;
    if (!q) return 0;
    typedef int (__cdecl *Init)(void);
    typedef int (__cdecl *Session)(void **);
    typedef int (__cdecl *Load)(void *);
    typedef int (__cdecl *Global)(void *, void **);
    typedef int (__cdecl *Destroy)(void *);
    Init init = (Init)q(0x0150E828);
    Session create = (Session)q(0x0694D52E);
    Load load = (Load)q(0x375DBD6B);
    Global global = (Global)q(0x617BFF9F);
    Destroy destroy = (Destroy)q(0xDAD9CFF8);
    void *session = NULL, *profile = NULL;
    if (!init || !create || !load || !global || !destroy || init() || create(&session)) return 0;
    int ok = !load(session) && !global(session, &profile);
    if (ok) {
        uint32_t on = 1;
        *found = nv_setting(q, session, profile, SETTING_CACHE_SIZE, size);
        *off = nv_setting(q, session, profile, SETTING_CACHE_ON, &on) && !on;
    }
    destroy(session);
    return ok;
}

static DWORD WINAPI check_main(void *arg) {
    int vulkan = (int)(intptr_t)arg;
    int found = 0, off = 0;
    uint32_t size = 0;
    if (!nv_read_settings(&found, &size, &off)) {
        fprintf(stderr, "[gfx] NVIDIA shader cache: the driver's settings could not be read (NvAPI)\n");
        return 0;
    }
    char base[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
    if (!n || n >= sizeof base) return 0;
    snprintf(path, sizeof path, "%s\\NVIDIA\\DXCache", base);
    uint64_t dx = dir_bytes(path);
    snprintf(path, sizeof path, "%s\\NVIDIA\\GLCache", base);
    uint64_t gl = dir_bytes(path);
    uint64_t own = vk_dir[0] ? dir_bytes(vk_dir) : 0;
    char words[32];
    nv_cache_setting_words(found, size, words, sizeof words);
    if (off) snprintf(words, sizeof words, "Disabled");
    char own_words[MAX_PATH + 64] = "";
    if (vk_dir[0]) snprintf(own_words, sizeof own_words, "; the game's own Vulkan set %.0f MB (%s)", own / 1048576.0, vk_dir);
    fprintf(stderr, "[gfx] NVIDIA shader cache: Shader Cache Size = %s (NVIDIA Control Panel); the driver's folders hold %.1f GB (DXCache, Direct3D) "
                    "and %.1f GB (GLCache, Vulkan)%s\n",
            words, dx / 1073741824.0, gl / 1073741824.0, own_words);
    /* What the renderer in use fills: DXCache for Direct3D; GLCache for Vulkan unless the game's folder holds its set. */
    uint64_t used = vulkan ? (vk_dir[0] ? 0 : gl) : dx;
    const char *folder = vulkan ? "GLCache" : "DXCache";
    const char *fix = "NVIDIA Control Panel > Manage 3D settings > Shader Cache Size: larger (100 GB or Unlimited)";
    if (off)
        fprintf(stderr, "[gfx] warning: NVIDIA's shader cache is Disabled: every start compiles the game's shaders again (hitches). %s\n", fix);
    else if (found && size && size != 0xFFFFFFFFu && used > (uint64_t)size << 20)
        fprintf(stderr, "[gfx] warning: NVIDIA's %s holds %.1f GB, over its Shader Cache Size of %s: the driver throws shaders away and compiles them "
                        "again during play (stutter). %s; or empty %%LOCALAPPDATA%%\\NVIDIA\\%s with no game open (every game rebuilds its own)\n",
                folder, used / 1073741824.0, words, fix, folder);
    else if ((!found || !size) && used > (4ull << 30))
        fprintf(stderr, "[gfx] note: NVIDIA's %s holds %.1f GB at the driver's default Shader Cache Size: if the game stutters, %s\n",
                folder, used / 1073741824.0, fix);
    return 0;
}

void nv_cache_check(int vulkan) {
    const char *on = getenv("SSBU_NV_CACHE_CHECK");
    if (on && !strcmp(on, "0")) return;
    HANDLE t = CreateThread(NULL, 0, check_main, (void *)(intptr_t)vulkan, 0, NULL);
    if (t) CloseHandle(t);
}

#else   /* (Linux and macOS: the driver's caches are not these) */

void nv_cache_vulkan_setup(const char *shader_cache_dir) { (void)shader_cache_dir; }
void nv_cache_check(int vulkan) { (void)vulkan; }
uint64_t nv_cache_trim(const char *dir, uint64_t cap, uint64_t keep_ms, uint32_t *removed, uint64_t *removed_bytes) {
    (void)dir; (void)cap; (void)keep_ms;
    if (removed) *removed = 0;
    if (removed_bytes) *removed_bytes = 0;
    return 0;
}

#endif
