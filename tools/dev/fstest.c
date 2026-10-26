/* Offline checks of the save data journal (runtime/hle_fs.c), through the nn::fs calls the game
 * makes, on made-up saves in a scratch folder (a made-up RomFS too: hle_fs.c opens one first).
 *
 *   Windows: clang-cl /nologo /O1 tools\dev\fstest.c /Febuild\fstest.exe   (with cc.BASE_CFLAGS)
 *   Linux:   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/fstest.c runtime/linux/win32.c -lpthread -o build/fstest
 *   macOS:   the same with runtime/macos/memory.c and runtime/macos/platform.c in place of -lpthread
 *   (runtime/macos/include: the Windows types without the Linux build's Direct3D headers)
 *
 *   fstest <folder> checks <a running process's id>
 *       the journal in one process: reads see the session's writes, which reach the committed
 *       folder only at Commit, and are gone after Unmount; Commit refused while a file is open
 *       for writing, not while files are open for reading; a save mounted once; listings and
 *       entry types of the merged save; deleting and making again, files becoming folders and
 *       back; leftovers of dead processes dropped (not a running one's); an incomplete journal
 *       ignored; a commit decided but not applied (a file the PC will not replace), finished
 *       later; read-only files; the cache and the RomFS read and written directly
 *   fstest <folder> old       puts the old save in the committed folder, nothing else
 *   fstest <folder> new       mounts it and changes it into the new save (files new, changed,
 *                             deleted, made again; folders new, deleted, made again), commits
 *                             and unmounts. SSBU_FS_CRASH_AT=n: ends the process (exit 99) at
 *                             the n-th step of the commit, as a crash would
 *   fstest <folder> verify    mounts the save (finishing a decided commit, or dropping an
 *                             undecided one), prints "old" or "new" - or "mixed" and what it
 *                             found - unmounts, and checks that nothing but the save is left
 *                             (no working folder, journal or marker). SSBU_FS_CRASH_AT=n: ends
 *                             the process at the n-th step of that recovery
 *
 * Exit code 0 when every check passes. tests/test_fs_journal.py runs them all, the crash steps
 * one by one. */
#define FS_TEST
#include <stdarg.h>
#include "../../runtime/hle_fs.c"
#ifndef _WIN32
#include <sys/stat.h>
#endif

/* ---- what hle_fs.c needs from the rest of the runtime --------------------------------- */
const char *rt_data_dir;
void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    exit(3);
}
void account_save_id(u64 uid[2]) { (void)uid; }

static int failures, checks;
static int is(const char *text, const char *want) { return text && !strcmp(text, want); }   /* (NULL: not there) */
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the nn::fs calls, as the game makes them ------------------------------------------ */
#define G(p) H2G(p)   /* (a host pointer as the guest address the calls take) */
static const u64 UID[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
static char folder[512], save_root[600];

static u32 call(void (*fn)(Cpu *), u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    static Cpu c;
    memset(&c, 0, sizeof c);
    c.x[0] = a0; c.x[1] = a1; c.x[2] = a2; c.x[3] = a3; c.x[4] = a4;
    fn(&c);
    return (u32)c.x[0];
}
static u32 mount_save(void) { return call(hle__ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE, G("save"), G(UID), 0, 0, 0); }
static void unmount(const char *name) { call(hle__ZN2nn2fs7UnmountEPKc, G(name), 0, 0, 0, 0); }
static u32 commit(const char *name) { return call(hle__ZN2nn2fs6CommitEPKc, G(name), 0, 0, 0, 0); }
static u32 t_create(const char *path, u64 size) { return call(hle__ZN2nn2fs10CreateFileEPKcl, G(path), size, 0, 0, 0); }
static u32 t_delete(const char *path) { return call(hle__ZN2nn2fs10DeleteFileEPKc, G(path), 0, 0, 0, 0); }
static u32 t_mkdir(const char *path) { return call(hle__ZN2nn2fs15CreateDirectoryEPKc, G(path), 0, 0, 0, 0); }
static u32 t_rmdir(const char *path) { return call(hle__ZN2nn2fs15DeleteDirectoryEPKc, G(path), 0, 0, 0, 0); }
static u32 t_open(const char *path, int mode, u64 *h) { return call(hle__ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, G(h), G(path), (u64)mode, 0, 0); }
static void t_close(u64 h) { call(hle__ZN2nn2fs9CloseFileENS0_10FileHandleE, h, 0, 0, 0, 0); }
static int t_type(const char *path) {   /* 0 folder, 1 file, -1 none */
    u32 type = 9;
    return call(hle__ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc, G(&type), G(path), 0, 0, 0) ? -1 : (int)type;
}
/* Writes `text` at `at` in the file (made first when `make`); keeps it open in *keep if asked. */
static u32 t_write(const char *path, u64 at, const char *text, int make, u64 *keep) {
    u64 h = 0;
    u32 r = make ? t_create(path, 0) : 0;
    if (!r) r = t_open(path, OPEN_READ | OPEN_WRITE, &h);
    if (r) return r;
    u32 option = 0;
    r = call(hle__ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE, h, at, G(text), strlen(text), G(&option));
    if (keep) *keep = h; else t_close(h);
    return r;
}
static const char *t_read_open(u64 h) {   /* the whole file an open handle reads, or NULL */
    static char buf[256];
    u64 size = 0, got = 0;
    call(hle__ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE, G(&size), h, 0, 0, 0);
    u32 r = size < sizeof buf ? call(hle__ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm, G(&got), h, 0, G(buf), size) : 1;
    if (r || got != size) return NULL;
    buf[got] = 0;
    return buf;
}
static const char *t_read(const char *path) {   /* the whole file, or NULL */
    u64 h = 0;
    if (t_open(path, OPEN_READ, &h)) return NULL;
    const char *text = t_read_open(h);
    t_close(h);
    return text;
}
/* A folder's listing, sorted: "a.bin:8 d1/ ..." */
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static const char *t_list(const char *path, int mode) {
    static char out[1024];
    static u8 entries[16][0x310];
    u64 h = 0, n = 0;
    out[0] = 0;
    if (call(hle__ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci, G(&h), G(path), (u64)mode, 0, 0)) return "(none)";
    call(hle__ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl, G(&n), G(entries), h, 16, 0);
    call(hle__ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE, h, 0, 0, 0, 0);
    char items[16][300], *sorted[16];
    for (u64 i = 0; i < n; i++) {
        u64 size;
        memcpy(&size, entries[i] + 0x308, 8);
        if (entries[i][0x304] == DIR_ENTRY) snprintf(items[i], sizeof items[i], "%s/", (char *)entries[i]);
        else snprintf(items[i], sizeof items[i], "%s:%llu", (char *)entries[i], (unsigned long long)size);
        sorted[i] = items[i];
    }
    qsort(sorted, (size_t)n, sizeof *sorted, by_name);
    for (u64 i = 0; i < n; i++) snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", i ? " " : "", sorted[i]);
    return out;
}

/* ---- the host folder, directly ---------------------------------------------------------- */
static void h_write(const char *path, const char *text) {
    make_parents(path);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { write_all(f, text, strlen(text)); CloseHandle(f); }
}
static void h_put(const char *rel, const char *text) {   /* a file of the committed save */
    char path[1024];
    join(path, sizeof path, save_root, rel);
    h_write(path, text);
}
static const char *h_get(const char *path) {
    static char buf[256];
    HANDLE f = CreateFileA(path, GENERIC_READ, SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    return buf;
}
static const char *h_save(const char *rel) {   /* a file of the committed save, or NULL */
    char path[1024];
    join(path, sizeof path, save_root, rel);
    return h_get(path);
}
static int h_exists(const char *path) { return attributes(path) != INVALID_FILE_ATTRIBUTES; }
static const char *h_beside(const char *suffix) {   /* <save>.<suffix> */
    static char path[1024];
    snprintf(path, sizeof path, "%s.%s", save_root, suffix);
    return path;
}
static const char *own(const char *kind) {   /* <save>.<kind>.<this process> */
    static char path[1024];
    snprintf(path, sizeof path, "%s.%s.%lu", save_root, kind, (unsigned long)GetCurrentProcessId());
    return path;
}

/* block(1): the PC refuses to replace a.bin of the committed save until block(0). Windows: another
 * program holds it open without FILE_SHARE_DELETE (and for longer than a commit tries again);
 * elsewhere the save's folder is read-only (not for root, whom that does not stop). */
#ifdef _WIN32
static HANDLE blocker = INVALID_HANDLE_VALUE;
static int can_block(void) { return 1; }
static void block(int on) {
    char path[1024];
    join(path, sizeof path, save_root, "a.bin");
    if (!on) { CloseHandle(blocker); return; }
    blocker = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(blocker != INVALID_HANDLE_VALUE, "test set-up: a.bin held open (error %lu)", (unsigned long)GetLastError());
}
static void read_only(const char *path) { SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY); }
#else
static int can_block(void) { return geteuid() != 0; }
static void block(int on) {
    char unix_root[1024];
    chmod(rt_unix_path(save_root, unix_root, sizeof unix_root), on ? 0555 : 0755);
}
static void read_only(const char *path) {
    char unix_path[1024];
    chmod(rt_unix_path(path, unix_path, sizeof unix_path), 0444);
}
#endif

/* The scratch folder: SSBU_DATA_ROOT, and a made-up RomFS - one file, hello.txt - in data/. */
static void setup(const char *dir) {
    char data[600], path[700];
    snprintf(folder, sizeof folder, "%s", dir);
#ifdef _WIN32
    _putenv_s("SSBU_DATA_ROOT", folder);
#else
    setenv("SSBU_DATA_ROOT", folder, 1);
#endif
    snprintf(save_root, sizeof save_root, "%s\\save\\%016llx%016llx", folder, UID[0], UID[1]);
    for (char *p = save_root; *p; p++) if (*p == '/') *p = '\\';
    snprintf(data, sizeof data, "%s/data", folder);
    make_dirs(data);
    static char data_dir[600];
    snprintf(data_dir, sizeof data_dir, "%s", data);
    rt_data_dir = data_dir;
    u8 image[0x99] = {0};
    u64 head[10] = {0x50, 0, 0, 0x50, 24, 0, 0, 0x68, 44, 0x94};
    RomDir root = {0, NONE, NONE, 0, 0, 0};
    struct { u32 parent, sibling; u64 offset, size; u32 hash, name_len; char name[12]; } file = {0, NONE, 0, 5, 0, 9, "hello.txt"};
    memcpy(image, head, sizeof head);
    memcpy(image + 0x50, &root, sizeof root);
    memcpy(image + 0x68, &file, sizeof file);
    memcpy(image + 0x94, "hello", 5);
    snprintf(path, sizeof path, "%s/rom.bin", data);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { write_all(f, image, sizeof image); CloseHandle(f); }
    char cfg[700], text[800];
    snprintf(cfg, sizeof cfg, "%s/romfs.cfg", data);
    snprintf(text, sizeof text, "%s\n0\n", path);
    f = CreateFileA(cfg, GENERIC_WRITE, SHARE_ALL, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { write_all(f, text, strlen(text)); CloseHandle(f); }
}

/* ---- in one process --------------------------------------------------------------------- */
static void checks_all(DWORD live_pid) {
    char path[1024];
    u64 h = 0;
    ready();

    /* the RomFS and the cache: no journal */
    CHECK(call(hle__ZN2nn2fs8MountRomEPKcPvm, G("rom"), 0, 0, 0, 0) == 0, "MountRom");
    CHECK(t_type("rom:/hello.txt") == 1 && t_read("rom:/hello.txt") && is(t_read("rom:/hello.txt"), "hello"), "a RomFS file");
    CHECK(t_open("rom:/hello.txt", OPEN_WRITE, &h) == R_UNSUPPORTED, "the RomFS opened for writing");
    CHECK(commit("rom") == 0, "Commit of the RomFS");
    CHECK(call(hle__ZN2nn2fs17MountCacheStorageEPKc, G("cache"), 0, 0, 0, 0) == 0, "MountCacheStorage");
    CHECK(t_write("cache:/c.bin", 0, "cached", 1, NULL) == 0, "a cache file");
    snprintf(path, sizeof path, "%s/cache/c.bin", folder);
    CHECK(h_get(path) && is(h_get(path), "cached"), "the cache is written directly: %s", h_get(path) ? h_get(path) : "(none)");
    CHECK(commit("cache") == 0 && commit("nothing") == R_NOT_MOUNTED, "Commit of the cache, and of no mount");

    /* the save: writes are the session's until Commit */
    h_put("a.bin", "AAAA-old"); h_put("b.bin", "BBBB-old"); h_put("d1\\c.bin", "CCCC-old"); h_put("d2\\e.bin", "EEEE-old");
    CHECK(mount_save() == 0 && mount_save() == R_MOUNT_EXISTS, "MountSaveData, then again");
    CHECK(call(hle__ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE, G("save2"), G(UID), 0, 0, 0) == R_MOUNT_EXISTS && t_type("save2:/") == -1,
          "the same save mounted under another name");
    CHECK(is_dir(own("work")), "the working folder %s", own("work"));
    CHECK(t_write("save:/a.bin", 0, "AAAA-new", 0, NULL) == 0, "a write");
    CHECK(t_read("save:/a.bin") && is(t_read("save:/a.bin"), "AAAA-new"), "reading back what was written: %s", t_read("save:/a.bin"));
    CHECK(is(h_save("a.bin"), "AAAA-old"), "an uncommitted write reached the committed folder");
    CHECK(t_write("save:/b.bin", 0, "XX", 0, NULL) == 0 && is(t_read("save:/b.bin"), "XXBB-old"), "a write over part of a file keeps the rest");
    CHECK(t_write("save:/f.bin", 0, "F", 1, NULL) == 0 && t_type("save:/f.bin") == 1, "a new file");
    CHECK(t_delete("save:/d2/e.bin") == 0 && t_type("save:/d2/e.bin") == -1 && t_delete("save:/d2/e.bin") == R_PATH_NOT_FOUND, "a deletion");
    CHECK(h_save("d2\\e.bin") != NULL, "an uncommitted deletion reached the committed folder");
    CHECK(t_mkdir("save:/d3") == 0 && t_write("save:/d3/g.bin", 0, "G", 1, NULL) == 0, "a new folder and a file in it");
    CHECK(t_type("save:/d3") == 0 && t_type("save:/") == 0 && t_type("save:/d1/c.bin") == 1, "entry types");
    CHECK(is(t_list("save:/", LIST_DIRS | LIST_FILES), "a.bin:8 b.bin:8 d1/ d2/ d3/ f.bin:1"), "the merged listing: %s", t_list("save:/", 3));
    CHECK(is(t_list("save:/", LIST_FILES), "a.bin:8 b.bin:8 f.bin:1") && is(t_list("save:/", LIST_DIRS), "d1/ d2/ d3/"), "files or folders only");
    CHECK(is(t_list("save:/d2", 3), "") && is(t_list("save:/d3", 3), "g.bin:1") && is(t_list("save:/nope", 3), "(none)"), "listings below");
    CHECK(t_create("save:/a.bin", 0) == R_PATH_EXISTS && t_create("save:/nope/x.bin", 0) == R_PATH_NOT_FOUND && t_mkdir("save:/d1") == R_PATH_EXISTS, "made where something is, or nothing");
    CHECK(t_rmdir("save:/d1") == R_PATH_NOT_FOUND && t_rmdir("save:/d2") == 0 && t_type("save:/d2") == -1, "only an empty folder is deleted");
    CHECK(t_open("save:/../x", OPEN_READ, &h) == R_PATH_NOT_FOUND, "a path out of the save");

    /* Commit is refused while a file is open for writing */
    CHECK(t_write("save:/a.bin", 0, "AAAA-new", 0, &h) == 0 && commit("save") == R_WRITE_FILE_OPEN, "Commit with a writer open");
    CHECK(is(h_save("a.bin"), "AAAA-old"), "the refused commit changed the committed folder");
    t_close(h);

    /* Unmount drops what was not committed */
    unmount("save");
    CHECK(!h_exists(own("work")), "the working folder after Unmount");
    CHECK(mount_save() == 0, "MountSaveData again");
    CHECK(is(t_read("save:/a.bin"), "AAAA-old") && t_type("save:/f.bin") == -1 && t_type("save:/d2/e.bin") == 1 && t_type("save:/d3") == -1,
          "uncommitted changes survived a remount");

    /* Commit, with files open for reading: one of the committed save (opened before its first
     * write, so the commit replaces the file it reads: Windows' MoveFileEx refuses that) and a
     * working copy (opened after it, so the commit moves the file it reads) */
    u64 old_reader = 0, new_reader = 0;
    CHECK(t_open("save:/b.bin", OPEN_READ, &old_reader) == 0, "a committed file opened for reading");
    t_write("save:/a.bin", 0, "AAAA-new", 0, NULL);
    t_write("save:/b.bin", 0, "XX", 0, NULL);
    t_write("save:/f.bin", 0, "F", 1, NULL);
    t_delete("save:/d2/e.bin");
    t_rmdir("save:/d2");
    t_mkdir("save:/d3");
    t_write("save:/d3/g.bin", 0, "G", 1, NULL);
    CHECK(t_open("save:/a.bin", OPEN_READ, &new_reader) == 0 && commit("save") == 0, "Commit with readers open");
    CHECK(is(t_read_open(new_reader), "AAAA-new") && is(t_read_open(old_reader), "BBBB-old"),
          "after Commit, a reader of a working copy reads the new file, one opened before the write the old one: %s", t_read_open(old_reader));
    t_close(old_reader);
    t_close(new_reader);
    CHECK(is(h_save("a.bin"), "AAAA-new") && is(h_save("b.bin"), "XXBB-old") && is(h_save("f.bin"), "F") && is(h_save("d3\\g.bin"), "G"),
          "the committed folder after Commit");
    snprintf(path, sizeof path, "%s\\d2", save_root);
    CHECK(!h_exists(path) && is(h_save("d1\\c.bin"), "CCCC-old"), "the deleted folder after Commit");
    CHECK(!h_exists(h_beside("commit")) && !h_exists(own("journal")) && folder_empty(own("work")), "the journal after Commit");
    CHECK(is(t_read("save:/a.bin"), "AAAA-new") && is(t_list("save:/", 3), "a.bin:8 b.bin:8 d1/ d3/ f.bin:1"), "the save after Commit: %s", t_list("save:/", 3));
    CHECK(commit("save") == 0 && !h_exists(own("journal")) && !h_exists(h_beside("commit")), "Commit with nothing to commit");

    /* deleted and made again; a file becomes a folder, a folder a file */
    t_delete("save:/d1/c.bin");
    t_rmdir("save:/d1");
    CHECK(t_mkdir("save:/d1") == 0 && t_type("save:/d1/c.bin") == -1 && is(t_list("save:/d1", 3), ""), "a folder deleted and made again is empty");
    t_write("save:/d1/c.bin", 0, "CCCC-new!", 1, NULL);
    CHECK(t_delete("save:/a.bin") == 0 && t_write("save:/a.bin", 0, "A2", 1, NULL) == 0 && is(t_read("save:/a.bin"), "A2"), "a file deleted and made again");
    t_delete("save:/f.bin");
    CHECK(t_mkdir("save:/f.bin") == 0 && t_write("save:/f.bin/in.bin", 0, "in", 1, NULL) == 0, "a file that becomes a folder");
    t_delete("save:/d3/g.bin");
    t_rmdir("save:/d3");
    CHECK(t_write("save:/d3", 0, "now a file", 1, NULL) == 0 && t_type("save:/d3") == 1, "a folder that becomes a file");
    CHECK(commit("save") == 0, "Commit");
    CHECK(is(h_save("d1\\c.bin"), "CCCC-new!") && is(h_save("a.bin"), "A2") && is(h_save("f.bin\\in.bin"), "in") && is(h_save("d3"), "now a file"),
          "made again, after Commit");
    unmount("save");

    /* at the next mount: leftovers of processes that ended are dropped, not a running one's */
    char dead_work[1024], dead_journal[1024], live_work[1024];
    snprintf(dead_work, sizeof dead_work, "%s.work.999999999", save_root);
    snprintf(dead_journal, sizeof dead_journal, "%s.journal.999999999", save_root);
    snprintf(live_work, sizeof live_work, "%s.work.%lu", save_root, (unsigned long)live_pid);
    join(path, sizeof path, dead_work, "a.bin");
    h_write(path, "LOST");
    h_write(dead_journal, "W\t999999999\n");
    make_dirs(live_work);
    CHECK(is_dir(dead_work) && h_exists(dead_journal), "test set-up: leftovers");
    CHECK(mount_save() == 0 && !h_exists(dead_work) && !h_exists(dead_journal) && is_dir(live_work), "leftovers after the next mount");
    CHECK(is(t_read("save:/a.bin"), "A2"), "a dead process's work reached the save");
    unmount("save");
    remove_tree(live_work);

    /* an incomplete marker (no end line) is ignored */
    join(path, sizeof path, dead_work, "a.bin");
    h_write(path, "LOST");
    h_write(h_beside("commit"), JOURNAL_FORMAT "\nW\t999999999\nF\ta.bin\n");
    CHECK(mount_save() == 0 && !h_exists(h_beside("commit")) && !h_exists(dead_work) && is(t_read("save:/a.bin"), "A2"), "an incomplete journal");
    unmount("save");

    /* a commit decided but not applied: the PC refuses to replace a.bin */
    if (can_block()) {
        CHECK(mount_save() == 0 && t_write("save:/a.bin", 0, "A3", 0, NULL) == 0, "a write");
        block(1);
        CHECK(commit("save") == R_HOST_FAILURE && h_exists(h_beside("commit")), "a commit not applied keeps its marker");
        CHECK(t_read("save:/a.bin") == NULL, "the save read while a decided commit waits");
        block(0);
        CHECK(is(t_read("save:/a.bin"), "A3") && is(h_save("a.bin"), "A3") && !h_exists(h_beside("commit")), "the next call finishes it");
        CHECK(t_write("save:/a.bin", 0, "A4", 0, NULL) == 0, "a write");
        block(1);
        CHECK(commit("save") == R_HOST_FAILURE, "a commit not applied");
        unmount("save");
        CHECK(is_dir(own("work")) && h_exists(h_beside("commit")), "Unmount keeps what a decided commit needs");
        block(0);
        CHECK(mount_save() == 0 && is(t_read("save:/a.bin"), "A4") && !h_exists(h_beside("commit")), "the next mount finishes it");
        unmount("save");
    }

    /* files marked read-only in the committed save are replaced and deleted all the same */
    h_put("ro1.bin", "RO1-old"); h_put("ro2.bin", "RO2-old");
    join(path, sizeof path, save_root, "ro1.bin");
    read_only(path);
    join(path, sizeof path, save_root, "ro2.bin");
    read_only(path);
    CHECK(mount_save() == 0 && t_write("save:/ro1.bin", 0, "RO1-new", 0, NULL) == 0 && t_delete("save:/ro2.bin") == 0, "a read-only file written, another deleted");
    CHECK(commit("save") == 0 && is(h_save("ro1.bin"), "RO1-new") && !h_save("ro2.bin") && !h_exists(h_beside("commit")), "Commit of read-only files");
    unmount("save");
}

/* ---- the crash matrix's two saves --------------------------------------------------------- */
static const char *OLD_SAVE = "chg1.bin=chg1-old-1111 chg2.bin=chg2-old-2222 dirx/ dirx\\inner.bin=inner-old diry/ diry\\y1.bin=y1-old diry\\y2.bin=y2-old "
                              "gone.bin=gone-old keep.bin=keep-old redo.bin=redo-old";
static const char *NEW_SAVE = "chg1.bin=chg1-new-1111 chg2.bin=chg2-new-2222 diry/ diry\\y1.bin=y1-new keep.bin=keep-old new.bin=new-file newdir/ "
                              "newdir\\n1.bin=n1-new newdir\\sub/ newdir\\sub\\n2.bin=n2-new redo.bin=redo-new-longer";

static void make_old(void) {
    char parent[1024];
    parent_of(parent, sizeof parent, save_root);
    remove_tree(parent);
    h_put("keep.bin", "keep-old"); h_put("chg1.bin", "chg1-old-1111"); h_put("chg2.bin", "chg2-old-2222"); h_put("gone.bin", "gone-old");
    h_put("redo.bin", "redo-old"); h_put("dirx\\inner.bin", "inner-old"); h_put("diry\\y1.bin", "y1-old"); h_put("diry\\y2.bin", "y2-old");
}

static int make_new(void) {
    ready();
    u32 r = mount_save();
    r |= t_write("save:/chg1.bin", 5, "new", 0, NULL);
    r |= t_write("save:/chg2.bin", 5, "new", 0, NULL);
    r |= t_delete("save:/gone.bin");
    r |= t_delete("save:/redo.bin");   /* (one call a statement: in this order) */
    r |= t_write("save:/redo.bin", 0, "redo-new-longer", 1, NULL);
    r |= t_delete("save:/dirx/inner.bin");
    r |= t_rmdir("save:/dirx");
    r |= t_delete("save:/diry/y1.bin");
    r |= t_delete("save:/diry/y2.bin");
    r |= t_rmdir("save:/diry");
    r |= t_mkdir("save:/diry");
    r |= t_write("save:/diry/y1.bin", 0, "y1-new", 1, NULL);
    r |= t_mkdir("save:/newdir");
    r |= t_write("save:/newdir/n1.bin", 0, "n1-new", 1, NULL);
    r |= t_mkdir("save:/newdir/sub");
    r |= t_write("save:/newdir/sub/n2.bin", 0, "n2-new", 1, NULL);
    r |= t_write("save:/new.bin", 0, "new-file", 1, NULL);
    if (r) { printf("making the new save failed: %x\n", r); return 1; }
    r = commit("save");
    unmount("save");
    if (r) { printf("Commit: %x\n", r); return 1; }
    return 0;
}

/* The mounted save, every entry: "a.bin=text dir/ dir\b.bin=text ..." (sorted). */
static void walk(char *out, size_t n, const char *rel) {
    u8 entries[32][0x310];
    char guest[600], sub[600];
    u64 h = 0, count = 0;
    snprintf(guest, sizeof guest, "save:/%s", rel);
    for (char *p = guest; *p; p++) if (*p == '\\') *p = '/';
    if (call(hle__ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci, G(&h), G(guest), 3, 0, 0)) return;
    call(hle__ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl, G(&count), G(entries), h, 32, 0);
    call(hle__ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE, h, 0, 0, 0, 0);
    for (u64 i = 0; i < count; i++) {
        join(sub, sizeof sub, rel, (char *)entries[i]);
        char item[1024];
        if (entries[i][0x304] == DIR_ENTRY) { snprintf(item, sizeof item, "%s/", sub); }
        else {
            snprintf(guest, sizeof guest, "save:/%s", sub);
            for (char *p = guest; *p; p++) if (*p == '\\') *p = '/';
            const char *text = t_read(guest);
            snprintf(item, sizeof item, "%s=%s", sub, text ? text : "(unreadable)");
        }
        snprintf(out + strlen(out), n - strlen(out), "%s\n", item);
        if (entries[i][0x304] == DIR_ENTRY) walk(out, n, sub);
    }
}
static int verify(void) {
    char all[4096] = "", *items[64], line[4096] = "";
    ready();
    u32 r = mount_save();
    if (r) { printf("MountSaveData: %x\n", r); return 1; }
    walk(all, sizeof all, "");
    unmount("save");
    int n = 0;
    for (char *s = strtok(all, "\n"); s && n < 64; s = strtok(NULL, "\n")) items[n++] = s;
    qsort(items, (size_t)n, sizeof *items, by_name);
    for (int i = 0; i < n; i++) snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", i ? " " : "", items[i]);
    int ok = 1;
    if (!strcmp(line, OLD_SAVE)) printf("old\n");
    else if (!strcmp(line, NEW_SAVE)) printf("new\n");
    else { printf("mixed: %s\n", line); ok = 0; }
    /* nothing but the save (and, on POSIX, the lock file) beside it */
    char parent[1024], pat[1100];
    parent_of(parent, sizeof parent, save_root);
    snprintf(pat, sizeof pat, "%s\\*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const char *leaf = strrchr(save_root, '\\') + 1;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || !strcmp(fd.cFileName, leaf)) continue;
            if (!strncmp(fd.cFileName, leaf, strlen(leaf)) && !strcmp(fd.cFileName + strlen(leaf), ".lock")) continue;
            printf("left behind: %s\n", fd.cFileName);
            ok = 0;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    return !ok;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fstest <folder> checks <pid> | old | new | verify\n"); return 2; }
    setup(argv[1]);
    if (!strcmp(argv[2], "old")) { make_old(); return 0; }
    if (!strcmp(argv[2], "new")) return make_new();
    if (!strcmp(argv[2], "verify")) return verify();
    if (strcmp(argv[2], "checks") || argc < 4) return 2;
    checks_all((DWORD)strtoul(argv[3], NULL, 10));
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
