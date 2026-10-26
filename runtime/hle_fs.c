/* nn::fs, natively.
 *
 *   rom:     the game's RomFS, read in place from its NCA (the section is not encrypted), so
 *            the 16.9 GB data.arc is served by positional reads with no extraction; or, with
 *            an update applied (tools/update.py), pieced together from several files by a
 *            layer map - the base's NCA and the update's own data
 *   save:    save data, a host folder per user (native-port/save/<user>), journaled as on the
 *            console: nothing the game writes reaches that folder before nn::fs::Commit, and a
 *            commit lands whole or not at all (see "the save data journal" below)
 *   cache:   cache storage, a host folder (native-port/cache), written directly
 *
 * Paths are "<mount>:/<path>". Handles are indices into a table; reads are positional, so
 * any number of game threads can read at once. Results use the SDK's file-system codes so
 * the game's own error handling (first boot: no save yet, and so on) takes its usual paths. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

#ifndef _WIN32   /* (the save journal's few calls Windows has no stand-in for in runtime/linux) */
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#define R_PATH_NOT_FOUND RESULT(2, 1)
#define R_PATH_EXISTS RESULT(2, 2)
#define R_OUT_OF_RANGE RESULT(2, 3005)
#define R_NOT_MOUNTED RESULT(2, 1001)
#define R_TOO_MANY_HANDLES RESULT(2, 6604)
#define R_INVALID_HANDLE RESULT(2, 6602)
#define R_MOUNT_EXISTS RESULT(2, 60)
#define R_UNSUPPORTED RESULT(2, 6300)
#define R_WRITE_FILE_OPEN RESULT(2, 6457)   /* Commit while a file of the mount is open for writing */
#define R_HOST_FAILURE RESULT(2, 500)       /* (no console counterpart: the PC refused a save file operation) */

enum { DIR_ENTRY = 0, FILE_ENTRY = 1 };          /* nn::fs::DirectoryEntryType */
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
enum { LIST_DIRS = 1, LIST_FILES = 2 };

/* ---- the RomFS -------------------------------------------------------------------- */
/* A layer: image bytes [at, the next layer's at) are source `src` from `off` on (LAYER_ZERO:
 * zeros). The map file (tools/nca.py write_layers): "SSBUROM1", u64 image size, u32 count,
 * u32 0, then the layers in image order. */
typedef struct { u64 at, off; u32 src, pad; } RomLayer;
#define LAYER_ZERO 0xFFFFFFFFu
#define ROM_SOURCES 8

static struct {
    HANDLE file;
    u64 base;           /* plain: where the RomFS image starts in the NCA */
    HANDLE sources[ROM_SOURCES];   /* layered: the files the layers come from */
    RomLayer *layers;
    u32 nlayers;
    u64 size;           /* layered: the image's size */
    u8 *dirs, *files;   /* directory and file metadata tables */
    u64 data;           /* file data offset within the image */
} rom;

typedef struct { u32 parent, sibling, child_dir, child_file, hash, name_len; char name[]; } RomDir;
typedef struct { u32 parent, sibling; u64 offset, size; u32 hash, name_len; char name[]; } RomFile;
#define NONE 0xFFFFFFFFu

static int pread_all(HANDLE h, u64 offset, void *buf, u64 size) {
    u8 *p = buf;
    while (size) {
        OVERLAPPED o = {0};
        o.Offset = (DWORD)offset;
        o.OffsetHigh = (DWORD)(offset >> 32);
        DWORD chunk = size > (1u << 30) ? (1u << 30) : (DWORD)size, got = 0;
        if (!ReadFile(h, p, chunk, &got, &o) || !got) return 0;
        p += got; offset += got; size -= got;
    }
    return 1;
}

/* Read image bytes [at, at + size). */
static int rom_read(u64 at, void *buf, u64 size) {
    if (!rom.layers) return pread_all(rom.file, rom.base + at, buf, size);
    if (at + size > rom.size) return 0;
    u32 lo = 0, hi = rom.nlayers;   /* the last layer starting at or before `at` */
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (rom.layers[mid].at <= at) lo = mid; else hi = mid;
    }
    u8 *p = buf;
    for (u32 k = lo; size; k++) {
        const RomLayer *l = &rom.layers[k];
        u64 end = k + 1 < rom.nlayers ? rom.layers[k + 1].at : rom.size;
        u64 n = end - at < size ? end - at : size;
        if (l->src == LAYER_ZERO) memset(p, 0, n);
        else if (!pread_all(rom.sources[l->src], l->off + (at - l->at), p, n)) return 0;
        p += n; at += n; size -= n;
    }
    return 1;
}

static HANDLE rom_file_open(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) rt_fatal("cannot open the game's RomFS at %s", path);
    return h;
}

/* romfs.cfg: "<file>\n<byte offset>\n" (the image in place in one file), or "layered\n<layer
 * map>\n<source 0>\n<source 1>...\n" (tools/update.py). */
static void rom_open(void) {
    char cfg[1024], path[1024];
    snprintf(cfg, sizeof cfg, "%s/romfs.cfg", rt_data_dir);
    FILE *f = fopen(cfg, "r");
    if (!f || !fgets(path, sizeof path, f)) rt_fatal("cannot read %s", cfg);
    path[strcspn(path, "\r\n")] = 0;
    if (!strcmp(path, "layered")) {
        char map[1024];
        if (!fgets(map, sizeof map, f)) rt_fatal("%s: no layer map", cfg);
        map[strcspn(map, "\r\n")] = 0;
        u32 n = 0;
        while (n < ROM_SOURCES && fgets(path, sizeof path, f)) {
            path[strcspn(path, "\r\n")] = 0;
            if (path[0]) rom.sources[n++] = rom_file_open(path);
        }
        HANDLE m = rom_file_open(map);
        u8 head[24];
        if (!pread_all(m, 0, head, sizeof head) || memcmp(head, "SSBUROM1", 8)) rt_fatal("%s: not a layer map", map);
        memcpy(&rom.size, head + 8, 8);
        memcpy(&rom.nlayers, head + 16, 4);
        rom.layers = malloc(sizeof *rom.layers * rom.nlayers);
        if (!rom.nlayers || !pread_all(m, sizeof head, rom.layers, sizeof *rom.layers * rom.nlayers))
            rt_fatal("%s: cannot read its %u layers", map, rom.nlayers);
        CloseHandle(m);
        for (u32 k = 0; k < rom.nlayers; k++)
            if (rom.layers[k].src != LAYER_ZERO && rom.layers[k].src >= n) rt_fatal("%s: layer %u names source %u of %u", map, k, rom.layers[k].src, n);
        fprintf(stderr, "[fs] RomFS: %llu bytes in %u layers from %u files (%s)\n", rom.size, rom.nlayers, n, cfg);
    } else {
        if (fscanf(f, "%llu", &rom.base) != 1) rt_fatal("cannot read %s", cfg);
        rom.file = rom_file_open(path);
    }
    fclose(f);
    u64 h[10];
    if (!rom_read(0, h, sizeof h) || h[0] != 0x50) rt_fatal("%s: no RomFS image there", cfg);
    rom.dirs = malloc(h[4]);
    rom.files = malloc(h[8]);
    if (!rom_read(h[3], rom.dirs, h[4]) || !rom_read(h[7], rom.files, h[8]))
        rt_fatal("cannot read the RomFS tables");
    rom.data = h[9];
}

static RomDir *rom_dir(u32 off) { return (RomDir *)(rom.dirs + off); }
static RomFile *rom_file(u32 off) { return (RomFile *)(rom.files + off); }

/* Resolve a RomFS path: sets *dir or *file (offsets into the tables); returns 0 if absent. */
static int rom_lookup(const char *path, u32 *dir, u32 *file) {
    u32 d = 0;
    *file = NONE;
    while (*path == '/') path++;
    while (*path) {
        size_t n = strcspn(path, "/");
        u32 next = NONE;
        for (u32 k = rom_dir(d)->child_dir; k != NONE; k = rom_dir(k)->sibling)
            if (rom_dir(k)->name_len == n && !memcmp(rom_dir(k)->name, path, n)) { next = k; break; }
        if (next == NONE) {
            if (path[n]) return 0;   /* only the last component may be a file */
            for (u32 k = rom_dir(d)->child_file; k != NONE; k = rom_file(k)->sibling)
                if (rom_file(k)->name_len == n && !memcmp(rom_file(k)->name, path, n)) { *file = k; return 1; }
            return 0;
        }
        d = next;
        path += n;
        while (*path == '/') path++;
    }
    *dir = d;
    return 1;
}

/* ---- mounts and handles ---------------------------------------------------------- */
typedef enum { M_ROM = 1, M_HOST, M_SAVE } MountKind;
static struct Mount {
    char name[32]; MountKind kind; char root[512];
    u32 id;                 /* tells this mount's handles from those of an earlier mount in the slot */
    char work[540];         /* save: the working folder (the journal, below) */
    char **tomb; int ntomb; /* save: paths deleted since the last commit */
    int pending;            /* save: a commit was decided but not all of it applied yet */
} mounts[16];

typedef struct Handle {
    int used, kind, mode;
    u32 mount;                /* the mount's id */
    HANDLE host;              /* host file */
    u64 offset, size;         /* rom file: data offset in the RomFS image and length */
    char **names; u8 *types; u64 *sizes; i64 count, next;   /* directory listing */
} Handle;
static Handle handles[512];
static CRITICAL_SECTION lock;      /* the mount and handle tables */
static CRITICAL_SECTION jlock;     /* save mounts' working state (taken before `lock`, never after) */
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_fs(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&jlock);
    rom_open();
    return TRUE;
}
static void ready(void) { InitOnceExecuteOnce(&once, init_fs, NULL, NULL); }

static struct Mount *find_mount(const char *path, const char **rest) {
    const char *colon = strchr(path, ':');
    if (!colon) return NULL;
    for (int i = 0; i < 16; i++)
        if (__atomic_load_n(&mounts[i].kind, __ATOMIC_ACQUIRE) && strlen(mounts[i].name) == (size_t)(colon - path) &&
            !memcmp(mounts[i].name, path, colon - path)) {
            *rest = colon + 1;
            return &mounts[i];
        }
    return NULL;
}

static struct Mount *mount_named(const char *name) {
    for (int i = 0; i < 16; i++)
        if (__atomic_load_n(&mounts[i].kind, __ATOMIC_ACQUIRE) && !strcmp(mounts[i].name, name)) return &mounts[i];
    return NULL;
}

static void make_dirs(const char *path) {   /* every folder of the chain */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++)
        if (*p == '/' || *p == '\\') { char k = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = k; }
    CreateDirectoryA(tmp, NULL);
}

static u32 add_mount(const char *name, MountKind kind, const char *root, const char *work) {
    static u32 serial;
    EnterCriticalSection(&lock);
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) { LeaveCriticalSection(&lock); return R_MOUNT_EXISTS; }
    for (int i = 0; i < 16; i++)
        if (!mounts[i].kind) {
            snprintf(mounts[i].name, sizeof mounts[i].name, "%s", name);
            snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
            snprintf(mounts[i].work, sizeof mounts[i].work, "%s", work ? work : "");
            mounts[i].tomb = NULL;
            mounts[i].ntomb = mounts[i].pending = 0;
            mounts[i].id = ++serial;
            if (kind == M_HOST) make_dirs(root);
            __atomic_store_n(&mounts[i].kind, kind, __ATOMIC_RELEASE);   /* (last: find_mount looks without the lock) */
            LeaveCriticalSection(&lock);
            return 0;
        }
    LeaveCriticalSection(&lock);
    return R_MOUNT_EXISTS;
}

static void host_path(char *out, size_t n, struct Mount *m, const char *rest) {
    while (*rest == '/') rest++;
    snprintf(out, n, "%s/%s", m->root, rest);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

static u64 new_handle(Handle h) {
    EnterCriticalSection(&lock);
    for (u32 i = 1; i < 512; i++)
        if (!handles[i].used) {
            handles[i] = h;
            handles[i].used = 1;
            LeaveCriticalSection(&lock);
            return 0x46530000ull | i;
        }
    LeaveCriticalSection(&lock);
    return 0;
}

static Handle *get_handle(u64 value) {
    u32 i = (u32)(value & 0xFFFF);
    return (value >> 16) == 0x4653 && i < 512 && handles[i].used ? &handles[i] : NULL;
}

static const char *data_root(char *out, size_t n, const char *leaf) {   /* native-port/<leaf>, or under SSBU_DATA_ROOT (test runs) */
    const char *root = getenv("SSBU_DATA_ROOT");
    if (root) snprintf(out, n, "%s/%s", root, leaf);
    else snprintf(out, n, "%s/../../%s", rt_data_dir, leaf);
    return out;
}

/* ---- the save data journal --------------------------------------------------------
 * On the console nothing the game writes to its save is kept until nn::fs::Commit, and a
 * commit is all or nothing: a crash or power cut mid-save leaves the last committed save.
 * Here, as in ssbu-recomp (runtime/nnfs.c):
 *   - save/<user> holds the committed save. Only a commit writes there.
 *   - Mounting makes the working folder save/<user>.work.<process id>. A file is copied
 *     there the first time it is opened for writing; new files and folders are made there;
 *     deletions are remembered (tombstones). Every open, listing and query sees the working
 *     folder over the committed one, without the deleted paths.
 *   - Commit flushes the changed files, writes the list of changes to the journal
 *     save/<user>.journal.<process id>, flushes it and renames it to save/<user>.commit: the
 *     commit is decided there. Then the changes are applied (deletions, new folders, each file
 *     moved over the committed one) and that marker deleted. Every step can be done again, so
 *     a process stopped after the rename has its commit finished by the next mount, by any
 *     process; one stopped before leaves the committed save as it was.
 *   - Unmount discards uncommitted changes, as the console does; so does the next mount, for
 *     a process that ended without unmounting.
 * Deletions are written with the kind of entry they delete (Dd a folder, Df a file), and only
 * for what the new save does not replace with the same kind: doing them again once the new
 * files are in place never touches those. (ssbu-recomp's journal deletes the old path whole,
 * so a commit that deletes a file or folder and makes it again lost the new one when it was
 * finished a second time.)
 * Moves and deletions try again for about a second when a file is held (an antivirus scan). On
 * Windows a committed file still open for reading is replaced as POSIX does it, and a read-only
 * mark is cleared first.
 * One process at a time commits or recovers: a lock file beside the save (save/<user>.lock),
 * on Windows a named mutex. One process mounts a save once. */
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)   /* (a commit can move a file someone reads) */
#define JOURNAL_FORMAT "ssbu-save-journal 1"   /* the journal's first line */
#ifndef ERROR_SHARING_VIOLATION
#define ERROR_SHARING_VIOLATION 32
#endif
#ifndef ERROR_LOCK_VIOLATION
#define ERROR_LOCK_VIOLATION 33
#endif

#ifdef _WIN32
typedef HANDLE SaveLock;
static SaveLock lock_saves(const char *root) {
    (void)root;
    HANDLE mx = CreateMutexA(NULL, FALSE, "Local\\ssbu-save-journal");
    if (mx) WaitForSingleObject(mx, INFINITE);   /* (WAIT_ABANDONED: its holder died; it is ours now) */
    return mx;
}
static void unlock_saves(SaveLock mx) { if (mx) { ReleaseMutex(mx); CloseHandle(mx); } }
static int pid_alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;   /* (one we may not open is running) */
    int alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}
/* NTFS logs a folder's changes in order, so none is on the disk without those before it (the
 * marker's rename without the flushed files it lists, its deletion without the moves). The log
 * goes to the disk within seconds, not when MoveFileEx returns (MOVEFILE_WRITE_THROUGH is
 * documented for moves done as a copy): a power cut just after a commit can bring back the save
 * before it, whole. */
static void flush_dir(const char *path) { (void)path; }
#else
typedef int SaveLock;
static SaveLock lock_saves(const char *root) {   /* (flock: the shim's objects have no names) */
    char path[1024], unix_path[1024];
    snprintf(path, sizeof path, "%s.lock", root);
    int fd = open(rt_unix_path(path, unix_path, sizeof unix_path), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) while (flock(fd, LOCK_EX) && errno == EINTR) {}
    return fd;
}
static void unlock_saves(SaveLock fd) { if (fd >= 0) close(fd); }
static int pid_alive(DWORD pid) { return pid && pid <= 0x7FFFFFFF && (kill((pid_t)pid, 0) == 0 || errno == EPERM); }
static void flush_dir(const char *path) {   /* the names in a folder: POSIX makes them durable only so */
    char unix_path[1024];
    int fd = open(rt_unix_path(path, unix_path, sizeof unix_path), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
#ifdef __APPLE__
    if (fcntl(fd, F_FULLFSYNC))   /* (fsync leaves it in the drive's cache) */
#endif
        fsync(fd);
    close(fd);
}
#endif

#ifdef FS_TEST
/* tools/dev/fstest.c: SSBU_FS_CRASH_AT=n ends the process at the n-th of these, as a crash would */
static void crash_point(void) {
    static int at = -1, count;
    if (at < 0) { const char *s = getenv("SSBU_FS_CRASH_AT"); at = s ? atoi(s) : 0; }
    if (++count != at) return;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 99);
#endif
    _exit(99);
}
#else
#define crash_point() ((void)0)
#endif

static DWORD attributes(const char *p) { return GetFileAttributesA(p); }
static int is_dir(const char *p) { DWORD a = attributes(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int is_file(const char *p) { DWORD a = attributes(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static void join(char *out, size_t n, const char *dir, const char *rel) {   /* (dir "": rel alone) */
    if (dir[0] && rel[0]) snprintf(out, n, "%s\\%s", dir, rel); else snprintf(out, n, "%s", dir[0] ? dir : rel);
}
static void make_parents(const char *path) {   /* the folders above `path` */
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *s = strrchr(tmp, '\\');
    if (s) { *s = 0; make_dirs(tmp); }
}
static void parent_of(char *out, size_t n, const char *path) {
    snprintf(out, n, "%s", path);
    char *s = strrchr(out, '\\');
    if (s) *s = 0; else snprintf(out, n, ".");
}

#ifdef _WIN32
/* Windows will not replace or delete a file marked read-only (a save copied from read-only
 * media): the mark goes first. */
static void not_read_only(const char *path) {
    DWORD a = GetFileAttributesA(path), b = a & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesA(path, b ? b : FILE_ATTRIBUTE_NORMAL);
}
/* MoveFileEx will not replace a file another handle has open, even one that shares deletion
 * (the game reading a committed file it then saves): ERROR_ACCESS_DENIED. A POSIX-style rename
 * does (NTFS, Windows 10 1607 and later), and that handle keeps reading the file it opened.
 * 0 with the move's own error if it cannot. */
typedef struct { DWORD flags; HANDLE root; DWORD len; WCHAR name[1024]; } RenameInfo;   /* FILE_RENAME_INFO, with Flags */
static int replace_open_file(const char *from, const char *to) {
    RenameInfo info = {.flags = 0x1 | 0x2};   /* FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS */
    DWORD err = GetLastError();
    int n = err == ERROR_ACCESS_DENIED ? MultiByteToWideChar(CP_ACP, 0, to, -1, info.name, 1024) : 0, ok = 0;
    HANDLE f = n > 0 ? CreateFileA(from, DELETE | SYNCHRONIZE, SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        info.len = (DWORD)(n - 1) * sizeof(WCHAR);
        ok = SetFileInformationByHandle(f, (FILE_INFO_BY_HANDLE_CLASS)22 /* FileRenameInfoEx */, &info,
                                        (DWORD)(FIELD_OFFSET(RenameInfo, name) + info.len + sizeof(WCHAR))) != 0;
        CloseHandle(f);
    }
    if (!ok) SetLastError(err);
    return ok;
}
#else
#define not_read_only(path) ((void)0)   /* (POSIX: a file's own permissions never stop a rename or a deletion) */
#define replace_open_file(from, to) 0   /* (rename replaces an open file) */
#endif

/* Windows: a file someone holds open (an antivirus scan, a backup) cannot be moved or deleted
 * for a moment - tried again for about a second. */
static int held(void) { DWORD e = GetLastError(); return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED; }
static int move_file(const char *from, const char *to) {
    not_read_only(to);
    for (DWORD wait = 1;; wait *= 2) {
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) || replace_open_file(from, to)) return 1;
        if (!held() || wait > 512) return 0;
        Sleep(wait);
    }
}
static int delete_file(const char *path) {
    not_read_only(path);
    for (DWORD wait = 1;; wait *= 2) {
        if (DeleteFileA(path) || !is_file(path)) return 1;
        if (!held() || wait > 512) return 0;
        Sleep(wait);
    }
}
static void remove_tree(const char *dir) {
    char pat[1024], sub[1024];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            join(sub, sizeof sub, dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(sub); else delete_file(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    not_read_only(dir);
    RemoveDirectoryA(dir);
}
static int folder_empty(const char *dir) {
    char pat[1024];
    WIN32_FIND_DATAA fd;
    int empty = 1;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do empty = !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..");
    while (empty && FindNextFileA(h, &fd));
    FindClose(h);
    return empty;
}
static int write_all(HANDLE h, const void *data, size_t n) {
    const u8 *p = data;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, put = 0;
        if (!WriteFile(h, p, chunk, &put, NULL) || !put) return 0;
        p += put; n -= put;
    }
    return 1;
}
static int copy_file(const char *from, const char *to) {
    HANDLE a = CreateFileA(from, GENERIC_READ, SHARE_ALL, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (a == INVALID_HANDLE_VALUE) return 0;
    HANDLE b = CreateFileA(to, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_ALWAYS, 0, NULL);
    int ok = b != INVALID_HANDLE_VALUE;
    u8 *buf = ok ? malloc(1 << 16) : NULL;
    for (ok = ok && buf;;) {
        DWORD got = 0;
        if (!ok || !ReadFile(a, buf, 1 << 16, &got, NULL)) { ok = 0; break; }
        if (!got) break;
        if (!write_all(b, buf, got)) { ok = 0; break; }
    }
    free(buf);
    CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    if (!ok) DeleteFileA(to);
    return ok;
}
static char *read_text(const char *path) {   /* the whole file, NUL-terminated; NULL if unreadable */
    HANDLE f = CreateFileA(path, GENERIC_READ, SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER s;
    char *t = NULL;
    if (GetFileSizeEx(f, &s) && s.QuadPart < (16 << 20) && (t = malloc((size_t)s.QuadPart + 1))) {
        if (s.QuadPart && !pread_all(f, 0, t, (u64)s.QuadPart)) { free(t); t = NULL; }
        else t[s.QuadPart] = 0;
    }
    CloseHandle(f);
    return t;
}

/* "save:/a//b/" -> "a\b", the path within the mount; 0 for one leaving it (".."). */
static int rel_path(char *out, size_t n, const char *rest) {
    size_t k = 0;
    for (;;) {
        while (*rest == '/' || *rest == '\\') rest++;
        size_t len = strcspn(rest, "/\\");
        if (!len) break;
        if (len == 2 && rest[0] == '.' && rest[1] == '.') return 0;
        if (!(len == 1 && rest[0] == '.')) {
            if (k + len + 2 > n) return 0;
            if (k) out[k++] = '\\';
            memcpy(out + k, rest, len);
            k += len;
        }
        rest += len;
    }
    out[k] = 0;
    return 1;
}

/* tombstones */
static int tomb_hides(struct Mount *m, const char *rel) {   /* rel, or a folder it is in, was deleted */
    for (int i = 0; i < m->ntomb; i++) {
        size_t n = strlen(m->tomb[i]);
        if (!_strnicmp(m->tomb[i], rel, n) && (rel[n] == 0 || rel[n] == '\\')) return 1;
    }
    return 0;
}
static void tomb_add(struct Mount *m, const char *rel) {
    for (int i = 0; i < m->ntomb; i++) if (!_stricmp(m->tomb[i], rel)) return;
    m->tomb = realloc(m->tomb, sizeof *m->tomb * (size_t)(m->ntomb + 1));
    m->tomb[m->ntomb++] = _strdup(rel);
}
static void tomb_clear(struct Mount *m) {
    for (int i = 0; i < m->ntomb; i++) free(m->tomb[i]);
    free(m->tomb);
    m->tomb = NULL;
    m->ntomb = 0;
}

/* Where `rel` is in the save as the game sees it: 2 a folder, 1 a file, 0 nowhere; `out` its
 * host path (the working copy first). */
static int view(struct Mount *m, const char *rel, char *out, size_t n) {
    join(out, n, m->work, rel);
    DWORD a = attributes(out);
    if (a == INVALID_FILE_ATTRIBUTES && !tomb_hides(m, rel)) { join(out, n, m->root, rel); a = attributes(out); }
    return a == INVALID_FILE_ATTRIBUTES ? 0 : a & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
}
static int view_parent_is_dir(struct Mount *m, const char *rel) {
    char parent[512], path[1024];
    snprintf(parent, sizeof parent, "%s", rel);
    char *s = strrchr(parent, '\\');
    if (!s) return 1;   /* (the mount's root) */
    *s = 0;
    return view(m, parent, path, sizeof path) == 2;
}

/* The journal's text. */
typedef struct { char *p; size_t n, cap; int bad, lines; } Text;
static void text_line(Text *t, const char *op, const char *arg) {
    size_t need = strlen(op) + strlen(arg) + 3;
    if (t->n + need + 1 > t->cap) {
        size_t cap = (t->cap + need) * 2;
        char *p = realloc(t->p, cap);
        if (!p) { t->bad = 1; return; }
        t->p = p;
        t->cap = cap;
    }
    if (arg[0]) t->n += (size_t)sprintf(t->p + t->n, "%s\t%s\n", op, arg);
    else t->n += (size_t)sprintf(t->p + t->n, "%s\n", op);
    t->lines++;
}

/* What a commit deletes under the deleted path `rel`: what the committed folder has there that
 * the working folder does not replace with the same kind of entry. */
static void journal_deletes(Text *t, struct Mount *m, const char *rel) {
    char r[1024], w[1024], pat[1024], sub[512];
    join(r, sizeof r, m->root, rel);
    join(w, sizeof w, m->work, rel);
    DWORD ra = attributes(r), wa = attributes(w);
    if (ra == INVALID_FILE_ATTRIBUTES) return;
    int rdir = (ra & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (wa == INVALID_FILE_ATTRIBUTES || rdir != ((wa & FILE_ATTRIBUTE_DIRECTORY) != 0)) { text_line(t, rdir ? "Dd" : "Df", rel); return; }
    if (!rdir) return;   /* (a file the new one replaces) */
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", r);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        join(sub, sizeof sub, rel, fd.cFileName);
        journal_deletes(t, m, sub);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Every folder (M) and file (F) of the working folder, parents first; each file flushed.
 * 0 if one could not be. */
static int journal_work(Text *t, const char *work, const char *rel) {
    char dir[1024], pat[1024], sub[512], full[1024];
    WIN32_FIND_DATAA fd;
    int ok = 1;
    join(dir, sizeof dir, work, rel);
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        join(sub, sizeof sub, rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            text_line(t, "M", sub);
            ok &= journal_work(t, work, sub);
        } else {
            join(full, sizeof full, work, sub);
            HANDLE f = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
            ok &= f != INVALID_HANDLE_VALUE && FlushFileBuffers(f);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            text_line(t, "F", sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    flush_dir(dir);
    return ok;
}

static int journal_rel_ok(const char *rel) {   /* (a marker names paths inside the save only) */
    char check[512];
    return rel[0] && !strchr(rel, ':') && rel[0] != '\\' && rel[0] != '/' && rel_path(check, sizeof check, rel) && !strcmp(check, rel);
}

/* Apply the decided commit in <root>.commit. Each step can be done again, so a commit cut
 * short is finished by running this once more. 1: done, the marker is gone (or there was none);
 * 0: the marker was incomplete and is dropped, the save stays as it was; -1: a step failed, the
 * marker stays. */
static int journal_apply(const char *root) {
    char marker[1024], parent[1024], work[1024] = "", a[1024], b[1024];
    snprintf(marker, sizeof marker, "%s.commit", root);
    parent_of(parent, sizeof parent, root);
    if (attributes(marker) == INVALID_FILE_ATTRIBUTES) return 1;
    char *text = read_text(marker);
    if (!text) return -1;
    size_t len = strlen(text), head = strlen(JOURNAL_FORMAT "\n");
    if (len < head + 2 || memcmp(text, JOURNAL_FORMAT "\n", head) || strcmp(text + len - 2, "E\n") || (len > head + 2 && text[len - 3] != '\n')) {
        free(text);
        fprintf(stderr, "[fs] %s: an incomplete save journal, ignored (the save stays as it was)\n", marker);
        if (!delete_file(marker)) return -1;
        flush_dir(parent);
        return 0;
    }
    int ok = 1;
    for (char *line = text + head, *next; ok && *line; line = next) {
        next = strchr(line, '\n');
        *next++ = 0;
        char *arg = strchr(line, '\t');
        if (arg) *arg++ = 0; else arg = line + strlen(line);
        if (!strcmp(line, "E")) break;
        if (!strcmp(line, "W")) {
            ok = arg[0] && strspn(arg, "0123456789") == strlen(arg);
            snprintf(work, sizeof work, "%s.work.%s", root, arg);
            continue;
        }
        if (!work[0] || !journal_rel_ok(arg)) { ok = 0; break; }
        join(a, sizeof a, root, arg);
        if (!strcmp(line, "Dd")) {   /* a folder that goes */
            if (is_dir(a)) remove_tree(a);
            ok = !is_dir(a);
            parent_of(b, sizeof b, a);
            flush_dir(b);
        } else if (!strcmp(line, "Df")) {   /* a file that goes */
            ok = !is_file(a) || delete_file(a);
            parent_of(b, sizeof b, a);
            flush_dir(b);
        } else if (!strcmp(line, "M")) {   /* a folder the save has now */
            if (!is_dir(a)) { make_parents(a); CreateDirectoryA(a, NULL); }
            ok = is_dir(a);
            parent_of(b, sizeof b, a);
            flush_dir(b);
        } else if (!strcmp(line, "F")) {   /* a file: the working copy moves over the committed one */
            join(b, sizeof b, work, arg);
            if (attributes(b) != INVALID_FILE_ATTRIBUTES) { make_parents(a); ok = move_file(b, a); }
        } else ok = 0;
        crash_point();
    }
    free(text);
    if (!ok) {
        fprintf(stderr, "[fs] %s: could not finish applying a save (error %lu); tried again later\n", marker, (unsigned long)GetLastError());
        return -1;
    }
    crash_point();
    if (!delete_file(marker)) return -1;
    flush_dir(parent);   /* (before anything else goes into the working folder: the marker must not be applied twice) */
    crash_point();
    return 1;
}

/* A commit applied: the working folder starts again empty. */
static void journal_done(struct Mount *m) {
    tomb_clear(m);
    remove_tree(m->work);
    CreateDirectoryA(m->work, NULL);
    m->pending = 0;
}

/* A decided commit of this mount that is not all applied: try it again (or note that another
 * process finished it). 1 when none is left. */
static int journal_settle(struct Mount *m) {
    if (!m->pending) return 1;
    SaveLock l = lock_saves(m->root);
    if (journal_apply(m->root) >= 0) journal_done(m);
    unlock_saves(l);
    return !m->pending;
}

/* Before a save is mounted: finish a commit that was decided, then drop the working folders
 * and half-written journals of processes that no longer run. */
static u32 journal_recover(const char *root) {
    char pat[1024], parent[1024], path[1024];
    const char *leaf = strrchr(root, '\\') ? strrchr(root, '\\') + 1 : root;
    size_t n = strlen(leaf);
    snprintf(path, sizeof path, "%s.commit", root);
    int decided = attributes(path) != INVALID_FILE_ATTRIBUTES, applied = journal_apply(root);
    if (applied < 0) return R_HOST_FAILURE;   /* (nothing is dropped while a commit still needs its files) */
    if (decided && applied) fprintf(stderr, "[fs] finished applying a save that was committed but cut short (%s)\n", root);
    snprintf(pat, sizeof pat, "%s.*", root);
    parent_of(parent, sizeof parent, root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *name = fd.cFileName, *pid = NULL;
        if (_strnicmp(name, leaf, n) || name[n] != '.') continue;
        if (!_strnicmp(name + n + 1, "work.", 5)) pid = name + n + 6;
        else if (!_strnicmp(name + n + 1, "journal.", 8)) pid = name + n + 9;
        if (!pid || !pid[0] || strlen(pid) > 10 || strspn(pid, "0123456789") != strlen(pid)) continue;
        unsigned long long id = strtoull(pid, NULL, 10);
        if (id > 0xFFFFFFFFull || (DWORD)id == GetCurrentProcessId() || pid_alive((DWORD)id)) continue;
        join(path, sizeof path, parent, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path); else delete_file(path);
        fprintf(stderr, "[fs] dropped %s, not committed by a run that ended (process %llu)\n", name, id);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static int writer_open(u32 mount) {   /* a file of the mount is open for writing */
    int found = 0;
    EnterCriticalSection(&lock);
    for (int i = 1; i < 512 && !found; i++)
        found = handles[i].used && handles[i].kind == FILE_ENTRY && handles[i].mount == mount && (handles[i].mode & OPEN_WRITE);
    LeaveCriticalSection(&lock);
    return found;
}

static u32 journal_commit(struct Mount *m) {
    char journal[1024], marker[1024], pid[16];
    if (writer_open(m->id)) return R_WRITE_FILE_OPEN;   /* ([sdk] FileSystemAccessor::Commit refuses too) */
    SaveLock l = lock_saves(m->root);
    if (journal_apply(m->root) < 0) { unlock_saves(l); return R_HOST_FAILURE; }   /* (a decided commit first, whichever process made it) */
    if (m->pending) journal_done(m);
    if (!m->ntomb && folder_empty(m->work)) { unlock_saves(l); return 0; }   /* (nothing changed) */
    snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
    snprintf(journal, sizeof journal, "%s.journal.%s", m->root, pid);
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    Text t = {0};
    text_line(&t, JOURNAL_FORMAT, "");
    text_line(&t, "W", pid);
    for (int i = 0; i < m->ntomb; i++) {
        int covered = 0;   /* (a deleted folder's walk has it) */
        for (int k = 0; k < m->ntomb && !covered; k++) {
            size_t n = strlen(m->tomb[k]);
            covered = k != i && !_strnicmp(m->tomb[k], m->tomb[i], n) && m->tomb[i][n] == '\\';
        }
        if (!covered) journal_deletes(&t, m, m->tomb[i]);
    }
    int ok = journal_work(&t, m->work, ""), changes = t.lines - 2;
    text_line(&t, "E", "");
    ok = ok && !t.bad;
    HANDLE f = ok ? CreateFileA(journal, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_ALWAYS, 0, NULL) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        ok = write_all(f, t.p, t.n / 2);
        crash_point();   /* (half the journal written) */
        ok = ok && write_all(f, t.p + t.n / 2, t.n - t.n / 2) && FlushFileBuffers(f);
        CloseHandle(f);
    } else ok = 0;
    free(t.p);
    crash_point();
    if (ok && !move_file(journal, marker)) ok = attributes(journal) == INVALID_FILE_ATTRIBUTES && attributes(marker) != INVALID_FILE_ATTRIBUTES;
    if (!ok) {   /* not decided: the committed save is as it was */
        fprintf(stderr, "[fs] commit of %s failed before it was decided (error %lu)\n", m->root, (unsigned long)GetLastError());
        DeleteFileA(journal);
        unlock_saves(l);
        return R_HOST_FAILURE;
    }
    crash_point();   /* (decided) */
    m->pending = 1;
    if (journal_apply(m->root) >= 0) journal_done(m);
    unlock_saves(l);
    if (!m->pending) fprintf(stderr, "[fs] save committed (%d changes)\n", changes);
    return m->pending ? R_HOST_FAILURE : 0;   /* (pending: every call on the mount tries it again first; the next mount finishes it) */
}

static int save_mounted(const char *root) {   /* (a second mount would share the first's working folder) */
    for (int i = 0; i < 16; i++)
        if (__atomic_load_n(&mounts[i].kind, __ATOMIC_ACQUIRE) == M_SAVE && !_stricmp(mounts[i].root, root)) return 1;
    return 0;
}

static u32 journal_mount(const char *name, const char *where) {
    char root[512], work[540], journal[540];
    snprintf(root, sizeof root, "%s", where);
    for (char *p = root; *p; p++) if (*p == '/') *p = '\\';
    EnterCriticalSection(&jlock);
    u32 r = mount_named(name) || save_mounted(root) ? R_MOUNT_EXISTS : 0;
    if (!r) {
        make_dirs(root);
        SaveLock l = lock_saves(root);
        r = journal_recover(root);
        if (!r) {   /* (left by a process that had this number before) */
            snprintf(work, sizeof work, "%s.work.%lu", root, (unsigned long)GetCurrentProcessId());
            snprintf(journal, sizeof journal, "%s.journal.%lu", root, (unsigned long)GetCurrentProcessId());
            remove_tree(work);
            DeleteFileA(journal);
            if (!CreateDirectoryA(work, NULL)) r = R_HOST_FAILURE;
        }
        unlock_saves(l);
        if (r) fprintf(stderr, "[fs] cannot mount the save in %s (error %lu)\n", root, (unsigned long)GetLastError());
        else r = add_mount(name, M_SAVE, root, work);
    }
    LeaveCriticalSection(&jlock);
    return r;
}

/* The save mount's own versions of the file calls; with jlock held. */
static u32 save_rel(struct Mount *m, const char *rest, char *rel, size_t n) {
    if (!journal_settle(m)) return R_HOST_FAILURE;
    return rel_path(rel, n, rest) ? 0 : R_PATH_NOT_FOUND;
}

static u32 save_open(struct Mount *m, const char *rest, Handle *h) {
    char rel[512], path[1024], w[1024];
    u32 r = save_rel(m, rest, rel, sizeof rel);
    if (r) return r;
    if (view(m, rel, path, sizeof path) != 1) return R_PATH_NOT_FOUND;
    if (h->mode & OPEN_WRITE) {   /* copy-on-write: a committed file is never opened for writing (one already open for
                                   * reading keeps reading the committed copy, without this handle's writes) */
        join(w, sizeof w, m->work, rel);
        if (strcmp(w, path)) {
            make_parents(w);
            if (!copy_file(path, w)) return R_HOST_FAILURE;
            snprintf(path, sizeof path, "%s", w);
        }
    }
    h->host = CreateFileA(path, GENERIC_READ | (h->mode & OPEN_WRITE ? GENERIC_WRITE : 0), SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    return h->host == INVALID_HANDLE_VALUE ? R_PATH_NOT_FOUND : 0;
}

static u32 save_create(struct Mount *m, const char *rest, i64 size, int dir) {
    char rel[512], path[1024];
    u32 r = save_rel(m, rest, rel, sizeof rel);
    if (r) return r;
    if (!rel[0] || view(m, rel, path, sizeof path)) return R_PATH_EXISTS;
    if (!view_parent_is_dir(m, rel)) return R_PATH_NOT_FOUND;
    join(path, sizeof path, m->work, rel);
    make_parents(path);
    if (dir) return CreateDirectoryA(path, NULL) ? 0 : R_HOST_FAILURE;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_NEW, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return R_HOST_FAILURE;
    LARGE_INTEGER s;
    s.QuadPart = size;
    SetFilePointerEx(f, s, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    CloseHandle(f);
    return 0;   /* (a deleted file of this name stays deleted: the commit deletes it, then moves this one in) */
}

static void listing_add(Handle *h, const char *name, size_t len, int type, u64 size);
static void listing_free(Handle *h);
/* A host folder's entries into a listing, without those a tombstone of `m` hides. `merge`: an
 * entry already listed (the committed copy of a working file) takes this one's size instead. */
static int listing_host(Handle *h, const char *dir, int mode, struct Mount *m, const char *rel, int merge) {
    char pat[1024], sub[512];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return 0;
    do {
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        u64 size = (u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || !(mode & (isdir ? LIST_DIRS : LIST_FILES))) continue;
        if (m) { join(sub, sizeof sub, rel, fd.cFileName); if (tomb_hides(m, sub)) continue; }
        i64 k = merge ? 0 : h->count;
        while (k < h->count && _stricmp(h->names[k], fd.cFileName)) k++;
        if (k < h->count) { h->types[k] = isdir ? DIR_ENTRY : FILE_ENTRY; h->sizes[k] = size; }
        else listing_add(h, fd.cFileName, strlen(fd.cFileName), isdir ? DIR_ENTRY : FILE_ENTRY, size);
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    return 1;
}

static u32 save_list(struct Mount *m, const char *rest, int mode, Handle *h) {   /* committed entries, then the working folder's */
    char rel[512], path[1024];
    u32 r = save_rel(m, rest, rel, sizeof rel);
    if (r) return r;
    if (view(m, rel, path, sizeof path) != 2) return R_PATH_NOT_FOUND;
    if (!tomb_hides(m, rel)) { join(path, sizeof path, m->root, rel); listing_host(h, path, mode, m, rel, 0); }
    join(path, sizeof path, m->work, rel);
    listing_host(h, path, mode, NULL, NULL, 1);
    return 0;
}

static u32 save_delete(struct Mount *m, const char *rest, int dir) {
    char rel[512], path[1024];
    u32 r = save_rel(m, rest, rel, sizeof rel);
    if (r) return r;
    if (!rel[0] || view(m, rel, path, sizeof path) != (dir ? 2 : 1)) return R_PATH_NOT_FOUND;
    if (dir) {   /* only an empty folder, as the host's own RemoveDirectory */
        Handle h = {0};
        save_list(m, rest, LIST_DIRS | LIST_FILES, &h);
        i64 n = h.count;
        listing_free(&h);
        if (n) return R_PATH_NOT_FOUND;
    }
    join(path, sizeof path, m->work, rel);
    if (dir ? is_dir(path) && !RemoveDirectoryA(path) : is_file(path) && !delete_file(path)) return R_HOST_FAILURE;
    join(path, sizeof path, m->root, rel);
    if (attributes(path) != INVALID_FILE_ATTRIBUTES) tomb_add(m, rel);
    return 0;
}

/* ---- mounting -------------------------------------------------------------------- */
HLE(_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE) { (void)c; ready(); }
HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { ready(); st64(c->x[0], 0x1000); c->x[0] = 0; }
HLE(_ZN2nn2fs8MountRomEPKcPvm) { ready(); c->x[0] = add_mount(P(char, c->x[0]), M_ROM, "", NULL); }
HLE(_ZN2nn2fs17MountCacheStorageEPKc) {
    char root[512];
    ready();
    c->x[0] = add_mount(P(char, c->x[0]), M_HOST, data_root(root, sizeof root, "cache"), NULL);
}
HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {
    char leaf[64], root[512];
    ready();
    u64 uid[2] = {ld64(c->x[1]), ld64(c->x[1] + 8)};
    account_save_id(uid);   /* (a netplay install's own id keeps the usual save) */
    snprintf(leaf, sizeof leaf, "save/%016llx%016llx", uid[0], uid[1]);
    c->x[0] = journal_mount(P(char, c->x[0]), data_root(root, sizeof root, leaf));
}
HLE(_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE) { ready(); c->x[0] = 0; }
HLE(_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE) { ready(); c->x[0] = 1; }
HLE(_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE) {
    st64(c->x[0], 64ull << 20);
    if (c->x[1]) st64(c->x[1], 64ull << 20);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs7UnmountEPKc) {   /* uncommitted changes to a save are discarded, as on the console */
    ready();
    const char *name = P(char, c->x[0]);
    EnterCriticalSection(&jlock);
    struct Mount *m = mount_named(name);
    if (m && m->kind == M_SAVE) {
        if (journal_settle(m)) remove_tree(m->work);
        else fprintf(stderr, "[fs] %s: a save is not all applied yet; finished at the next start\n", m->root);
        tomb_clear(m);
    }
    EnterCriticalSection(&lock);
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) mounts[i].kind = 0;
    LeaveCriticalSection(&lock);
    LeaveCriticalSection(&jlock);
}
HLE(_ZN2nn2fs6CommitEPKc) {
    ready();
    EnterCriticalSection(&jlock);
    struct Mount *m = mount_named(P(char, c->x[0]));
    c->x[0] = !m ? R_NOT_MOUNTED : m->kind == M_SAVE ? journal_commit(m) : 0;   /* (the cache is written directly, the RomFS never) */
    LeaveCriticalSection(&jlock);
}

/* ---- queries --------------------------------------------------------------------- */
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f)) { c->x[0] = R_PATH_NOT_FOUND; return; }
        st32(c->x[0], f == NONE ? DIR_ENTRY : FILE_ENTRY);
    } else if (m->kind == M_SAVE) {
        char rel[512], path[1024];
        EnterCriticalSection(&jlock);
        u32 r = save_rel(m, rest, rel, sizeof rel);
        int type = r ? 0 : view(m, rel, path, sizeof path);
        LeaveCriticalSection(&jlock);
        if (r || !type) { c->x[0] = r ? r : R_PATH_NOT_FOUND; return; }
        st32(c->x[0], type == 2 ? DIR_ENTRY : FILE_ENTRY);
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        DWORD a = GetFileAttributesA(path);
        if (a == INVALID_FILE_ATTRIBUTES) { c->x[0] = R_PATH_NOT_FOUND; return; }
        st32(c->x[0], a & FILE_ATTRIBUTE_DIRECTORY ? DIR_ENTRY : FILE_ENTRY);
    }
    c->x[0] = 0;
}

/* ---- files ----------------------------------------------------------------------- */
HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    int mode = (int)c->x[2];
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    Handle h = {0};
    h.kind = FILE_ENTRY;
    h.mode = mode;
    h.mount = m->id;
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f) || f == NONE) { c->x[0] = R_PATH_NOT_FOUND; return; }
        if (mode & OPEN_WRITE) { c->x[0] = R_UNSUPPORTED; return; }
        h.offset = rom.data + rom_file(f)->offset;
        h.size = rom_file(f)->size;
    } else if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);   /* (held until the handle is in the table: Commit looks for writers there) */
        u32 r = save_open(m, rest, &h);
        u64 v = r ? 0 : new_handle(h);
        if (!r && !v) { CloseHandle(h.host); r = R_TOO_MANY_HANDLES; }
        LeaveCriticalSection(&jlock);
        if (r) { c->x[0] = r; return; }
        st64(c->x[0], v);
        c->x[0] = 0;
        return;
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        h.host = CreateFileA(path, GENERIC_READ | (mode & OPEN_WRITE ? GENERIC_WRITE : 0), SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
        if (h.host == INVALID_HANDLE_VALUE) { c->x[0] = R_PATH_NOT_FOUND; return; }
    }
    u64 v = new_handle(h);
    if (!v) { if (h.host) CloseHandle(h.host); c->x[0] = R_TOO_MANY_HANDLES; return; }
    st64(c->x[0], v);
    c->x[0] = 0;
}

static u64 file_size(Handle *h) {
    if (!h->host) return h->size;
    LARGE_INTEGER s;
    GetFileSizeEx(h->host, &s);
    return (u64)s.QuadPart;
}

/* Read up to `size` bytes at `offset`; returns the count, or ~0 on a host error. */
static u64 read_at(Handle *h, i64 offset, void *buf, u64 size) {
    u64 total = file_size(h);
    if (offset < 0 || (u64)offset > total) return ~0ull;
    if (size > total - (u64)offset) size = total - (u64)offset;
    if (!size) return 0;
    int ok = h->host ? pread_all(h->host, (u64)offset, buf, size) : rom_read(h->offset + (u64)offset, buf, size);
    return ok ? size : ~0ull;
}

HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {   /* (handle, offset, buffer, size): all or nothing */
    Handle *h = get_handle(c->x[0]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    u64 n = read_at(h, (i64)c->x[1], P(void, c->x[2]), c->x[3]);
    c->x[0] = n == c->x[3] ? 0 : R_OUT_OF_RANGE;
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {  /* (read*, handle, offset, buffer, size) */
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    u64 n = read_at(h, (i64)c->x[2], P(void, c->x[3]), c->x[4]);
    if (n == ~0ull) { c->x[0] = R_OUT_OF_RANGE; return; }
    st64(c->x[0], n);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {   /* (handle, offset, data, size, option) */
    Handle *h = get_handle(c->x[0]);
    if (!h || !h->host || !(h->mode & OPEN_WRITE)) { c->x[0] = R_INVALID_HANDLE; return; }
    OVERLAPPED o = {0};
    o.Offset = (DWORD)c->x[1];
    o.OffsetHigh = (DWORD)(c->x[1] >> 32);
    DWORD put = 0;
    WriteFile(h->host, P(void, c->x[2]), (DWORD)c->x[3], &put, &o);
    c->x[0] = put == (DWORD)c->x[3] ? 0 : R_OUT_OF_RANGE;
}
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    st64(c->x[0], file_size(h));
    c->x[0] = 0;
}
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) {
    Handle *h = get_handle(c->x[0]);
    if (!h) return;
    if (h->host) CloseHandle(h->host);
    EnterCriticalSection(&lock);
    memset(h, 0, sizeof *h);
    LeaveCriticalSection(&lock);
}
HLE(_ZN2nn2fs10CreateFileEPKcl) {   /* (path, size) */
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind == M_ROM) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);
        c->x[0] = save_create(m, rest, (i64)c->x[1], 0);
        LeaveCriticalSection(&jlock);
        return;
    }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_NEW, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { c->x[0] = GetLastError() == ERROR_FILE_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND; return; }
    LARGE_INTEGER size;
    size.QuadPart = (i64)c->x[1];
    SetFilePointerEx(f, size, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    CloseHandle(f);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs10DeleteFileEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind == M_ROM) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);
        c->x[0] = save_delete(m, rest, 0);
        LeaveCriticalSection(&jlock);
        return;
    }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = DeleteFileA(path) ? 0 : R_PATH_NOT_FOUND;
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind == M_ROM) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);
        c->x[0] = save_create(m, rest, 0, 1);
        LeaveCriticalSection(&jlock);
        return;
    }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = CreateDirectoryA(path, NULL) ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND;
}
HLE(_ZN2nn2fs15DeleteDirectoryEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind == M_ROM) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);
        c->x[0] = save_delete(m, rest, 1);
        LeaveCriticalSection(&jlock);
        return;
    }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = RemoveDirectoryA(path) ? 0 : R_PATH_NOT_FOUND;
}

/* ---- directories ----------------------------------------------------------------- */
static void listing_add(Handle *h, const char *name, size_t len, int type, u64 size) {
    h->names = realloc(h->names, sizeof *h->names * (h->count + 1));
    h->types = realloc(h->types, h->count + 1);
    h->sizes = realloc(h->sizes, sizeof *h->sizes * (h->count + 1));
    h->names[h->count] = malloc(len + 1);
    memcpy(h->names[h->count], name, len);
    h->names[h->count][len] = 0;
    h->types[h->count] = (u8)type;
    h->sizes[h->count++] = size;
}
static void listing_free(Handle *h) {
    for (i64 i = 0; i < h->count; i++) free(h->names[i]);
    free(h->names); free(h->types); free(h->sizes);
}

HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {   /* (handle*, path, mode) */
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    int mode = (int)c->x[2];
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    Handle h = {0};
    h.kind = DIR_ENTRY;
    h.mount = m->id;
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f) || f != NONE) { c->x[0] = R_PATH_NOT_FOUND; return; }
        if (mode & LIST_DIRS)
            for (u32 k = rom_dir(d)->child_dir; k != NONE; k = rom_dir(k)->sibling)
                listing_add(&h, rom_dir(k)->name, rom_dir(k)->name_len, DIR_ENTRY, 0);
        if (mode & LIST_FILES)
            for (u32 k = rom_dir(d)->child_file; k != NONE; k = rom_file(k)->sibling)
                listing_add(&h, rom_file(k)->name, rom_file(k)->name_len, FILE_ENTRY, rom_file(k)->size);
    } else if (m->kind == M_SAVE) {
        EnterCriticalSection(&jlock);
        u32 r = save_list(m, rest, mode, &h);
        LeaveCriticalSection(&jlock);
        if (r) { listing_free(&h); c->x[0] = r; return; }
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        if (!listing_host(&h, path, mode, NULL, NULL, 0)) { c->x[0] = R_PATH_NOT_FOUND; return; }
    }
    u64 v = new_handle(h);
    if (!v) { listing_free(&h); c->x[0] = R_TOO_MANY_HANDLES; return; }
    st64(c->x[0], v);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {   /* (count*, entries, handle, max) */
    Handle *h = get_handle(c->x[2]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    i64 n = 0;
    for (; n < (i64)c->x[3] && h->next < h->count; n++, h->next++) {   /* DirectoryEntry: 0x310 bytes */
        u64 e = c->x[1] + 0x310ull * (u64)n;
        memset(P(void, e), 0, 0x310);
        strncpy(P(char, e), h->names[h->next], 0x300);
        st8(e + 0x304, h->types[h->next]);
        st64(e + 0x308, h->sizes[h->next]);
    }
    st64(c->x[0], (u64)n);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    st64(c->x[0], (u64)h->count);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    Handle *h = get_handle(c->x[0]);
    if (!h) return;
    listing_free(h);
    EnterCriticalSection(&lock);
    memset(h, 0, sizeof *h);
    LeaveCriticalSection(&lock);
}
