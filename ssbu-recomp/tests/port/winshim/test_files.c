/* test_files.c - plat_posix.c's file calls where Linux and macOS behave unlike Windows unless the stand-in layer makes them:
 *   FindFirstFileA / FindNextFileA  "." and ".." first, then NTFS's order (upper-case ordinal), not readdir's; Win32 wildcards
 *                                   ('*', '?' as one character, no [ ] classes), ignoring case; the errors of an empty search
 *   MoveFileExA                     without MOVEFILE_REPLACE_EXISTING nothing at the new name is replaced, also when 8 threads
 *                                   move onto one name at once (a look-then-rename lets several of them replace each other)
 *   CopyFileA                       a folder as the source fails with ERROR_ACCESS_DENIED before the target is made or touched
 *   MapViewOfFile / UnmapViewOfFile 8 threads mapping and unmapping views at once, as nvn_vk_store.c's workers do: every view
 *                                   unmaps and shows its own bytes; a full table refuses the next map
 * Everything happens in a fresh folder under $TMPDIR (/tmp), removed at the end. No game data. */
#include "tp.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_dir[512];

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s\\%s", g_dir, name); }
static int put_file(const char *name, const char *text) {
    char p[600];
    path_of(p, sizeof p, name);
    FILE *f = fopen(plat_path(p, p, sizeof p), "wb");
    if (!f) return 0;
    fputs(text, f);
    return fclose(f) == 0;
}
static int file_is(const char *name, const char *text) {           /* exists and holds exactly `text` */
    char p[600], b[256];
    path_of(p, sizeof p, name);
    FILE *f = fopen(plat_path(p, p, sizeof p), "rb");
    if (!f) return 0;
    size_t k = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[k] = 0;
    return !strcmp(b, text);
}
static int exists(const char *name) { char p[600]; path_of(p, sizeof p, name); return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void remove_tree(const char *dir) {
    char pat[600], sub[600];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(sub); else DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* the names a search gives, joined with '|' */
static int list(const char *pattern, char *out, size_t n, DWORD *err) {
    char pat[600];
    WIN32_FIND_DATAA fd;
    path_of(pat, sizeof pat, pattern);
    out[0] = 0;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return 0; }
    size_t k = 0;
    do k += (size_t)snprintf(out + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", fd.cFileName);
    while (FindNextFileA(h, &fd));
    *err = GetLastError();
    FindClose(h);
    return 1;
}
static void test_find(void) {
    SECTION("FindFirstFileA: '.' and '..' first, then upper-case ordinal order, whatever order the disk keeps");
    /* made-up names: punctuation on both sides of the letters, both cases, a bracket, two-byte and three-byte UTF-8 */
    static const char *names[] = { "b.dll", "_c.dll", "z.txt", "A.DLL", "-d.dll", "[e].dll", "\xc3\xa9.dll", "\xef\xbc\xa1.dll" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) CHECK(put_file(names[i], names[i]));
    char sub[600];
    path_of(sub, sizeof sub, "Sub");
    CHECK(CreateDirectoryA(sub, NULL));
    char got[2048];
    DWORD err = 0;
    static const struct { const char *pat, *want; } cases[] = {
        { "*",       ".|..|-d.dll|A.DLL|b.dll|Sub|z.txt|[e].dll|_c.dll|\xc3\xa9.dll|\xef\xbc\xa1.dll" },
        { "*.*",     ".|..|-d.dll|A.DLL|b.dll|Sub|z.txt|[e].dll|_c.dll|\xc3\xa9.dll|\xef\xbc\xa1.dll" },
        { "*.dll",   "-d.dll|A.DLL|b.dll|[e].dll|_c.dll|\xc3\xa9.dll|\xef\xbc\xa1.dll" },      /* ignoring case: A.DLL too */
        { "?.dll",   "A.DLL|b.dll|\xc3\xa9.dll|\xef\xbc\xa1.dll" },                               /* '?' is one character, not one byte */
        { "[e]*",    "[e].dll" },                                                               /* a literal bracket, not a class */
        { "s*",      "Sub" },
        { "*D*.?LL", "-d.dll" },                                                                   /* the last '*' backs off */
        { "b.dll",   "b.dll" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        int ok = list(cases[i].pat, got, sizeof got, &err);
        CHECKF(ok && !strcmp(got, cases[i].want), "pattern %s: got \"%s\", want \"%s\"", cases[i].pat, got, cases[i].want);
        CHECKF(!ok || err == ERROR_NO_MORE_FILES, "pattern %s: the end of the search sets error %u", cases[i].pat, err);
    }
    WIN32_FIND_DATAA fd;
    char pat[600];
    path_of(pat, sizeof pat, "S*");
    HANDLE h = FindFirstFileA(pat, &fd);
    CHECK(h != INVALID_HANDLE_VALUE && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    SECTION("FindFirstFileA over 300 names (more than one read of the folder): all of them, in order");
    char nm[32];
    for (int i = 299; i >= 0; i--) { snprintf(nm, sizeof nm, i & 1 ? "N%03d" : "n%03d", i); put_file(nm, nm); }
    int seen = 0, sorted = 1, prev = -1;
    path_of(pat, sizeof pat, "n*");
    HANDLE hn = FindFirstFileA(pat, &fd);
    if (hn != INVALID_HANDLE_VALUE) {
        do { int k = atoi(fd.cFileName + 1); sorted &= k == prev + 1; prev = k; seen++; } while (FindNextFileA(hn, &fd));
        FindClose(hn);
    }
    CHECKF(seen == 300 && sorted, "%d names, %s", seen, sorted ? "in order" : "out of order");
    for (int i = 0; i < 300; i++) { char p[600]; snprintf(nm, sizeof nm, i & 1 ? "N%03d" : "n%03d", i); path_of(p, sizeof p, nm); DeleteFileA(p); }

    SECTION("FindFirstFileA: no match is ERROR_FILE_NOT_FOUND, a missing folder ERROR_PATH_NOT_FOUND");
    CHECKF(!list("nothing*", got, sizeof got, &err) && err == ERROR_FILE_NOT_FOUND, "error %u", err);
    CHECKF(!list("missing\\*", got, sizeof got, &err) && err == ERROR_PATH_NOT_FOUND, "error %u", err);
    RemoveDirectoryA(sub);
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) { char p[600]; path_of(p, sizeof p, names[i]); DeleteFileA(p); }
}

static void test_move(void) {
    char a[600], b[600];
    SECTION("MoveFileExA without MOVEFILE_REPLACE_EXISTING: a free name moves, an existing one fails with ERROR_ALREADY_EXISTS");
    path_of(a, sizeof a, "m_a"); path_of(b, sizeof b, "m_b");
    CHECK(put_file("m_a", "A"));
    CHECK(MoveFileExA(a, b, 0) && !exists("m_a") && file_is("m_b", "A"));
    CHECK(put_file("m_a", "A2"));
    DWORD flags[2] = { 0, MOVEFILE_WRITE_THROUGH };
    for (int i = 0; i < 2; i++) {
        SetLastError(0);
        CHECKF(!MoveFileExA(a, b, flags[i]) && GetLastError() == ERROR_ALREADY_EXISTS, "flags %u: error %u", flags[i], GetLastError());
        CHECKF(file_is("m_a", "A2") && file_is("m_b", "A"), "flags %u: a file changed", flags[i]);
    }
    SECTION("MoveFileExA with MOVEFILE_REPLACE_EXISTING replaces");
    CHECK(MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) && !exists("m_a") && file_is("m_b", "A2"));
    DeleteFileA(b);

    SECTION("MoveFileExA of a folder: onto an empty folder fails with ERROR_ALREADY_EXISTS, to a free name moves");
    path_of(a, sizeof a, "d_a"); path_of(b, sizeof b, "d_b");
    CHECK(CreateDirectoryA(a, NULL) && CreateDirectoryA(b, NULL));
    SetLastError(0);
    CHECKF(!MoveFileExA(a, b, 0) && GetLastError() == ERROR_ALREADY_EXISTS, "error %u", GetLastError());
    CHECK(exists("d_a") && exists("d_b"));
    CHECK(RemoveDirectoryA(b) && MoveFileExA(a, b, 0) && !exists("d_a") && exists("d_b"));
    RemoveDirectoryA(b);

    SECTION("MoveFileExA: a change of case only moves (also where the file system ignores case)");
    path_of(a, sizeof a, "case.txt"); path_of(b, sizeof b, "CASE.txt");
    CHECK(put_file("case.txt", "c"));
    CHECK(MoveFileExA(a, b, 0) && file_is("CASE.txt", "c"));
    char got[256]; DWORD err;
    CHECK(list("case.txt", got, sizeof got, &err) && !strcmp(got, "CASE.txt"));
    DeleteFileA(b);
}

/* 8 threads move their own file onto one name at once, 200 rounds: exactly one move wins each round, the name holds the
 * winner's file and every other file is still where it was (checking first, then renaming, lets several moves "win", each
 * replacing the one before) */
typedef struct { volatile LONG *wins, *errs, *go; int id; } RaceArg;
static DWORD WINAPI race_one(LPVOID p) {
    RaceArg *a = p;
    char src[600], dst[600], nm[32];
    snprintf(nm, sizeof nm, "race_%d", a->id);
    path_of(src, sizeof src, nm);
    path_of(dst, sizeof dst, "race_target");
    while (!LOAD(*a->go)) {}
    if (MoveFileExA(src, dst, 0)) InterlockedIncrement(a->wins);
    else if (GetLastError() != ERROR_ALREADY_EXISTS) InterlockedIncrement(a->errs);
    return 0;
}
static void test_move_race(void) {
    SECTION("MoveFileExA without the flag from 8 threads onto one name (200 rounds): one move wins, nothing is replaced");
    char dst[600], nm[32], p[600];
    path_of(dst, sizeof dst, "race_target");
    int bad = 0, lost = 0;
    for (int r = 0; r < 200; r++) {
        for (int i = 0; i < 8; i++) { snprintf(nm, sizeof nm, "race_%d", i); put_file(nm, nm); }
        DeleteFileA(dst);
        volatile LONG wins = 0, errs = 0, go = 0;
        RaceArg args[8];
        HANDLE h[8];
        for (int i = 0; i < 8; i++) { args[i] = (RaceArg){ &wins, &errs, &go, i }; h[i] = CreateThread(NULL, 0, race_one, &args[i], 0, NULL); }
        InterlockedExchange(&go, 1);
        WaitForMultipleObjects(8, h, TRUE, INFINITE);
        for (int i = 0; i < 8; i++) CloseHandle(h[i]);
        int left = 0, moved = -1;
        for (int i = 0; i < 8; i++) { snprintf(nm, sizeof nm, "race_%d", i); if (exists(nm)) left++; else moved = i; }
        if (wins != 1 || errs || left != 7) bad++;
        if (moved < 0 || (snprintf(nm, sizeof nm, "race_%d", moved), !file_is("race_target", nm))) lost++;
    }
    CHECKF(bad == 0, "%d of 200 rounds: more than one move succeeded (each replacing the last), or one failed otherwise", bad);
    CHECKF(lost == 0, "%d of 200 rounds: the target does not hold the moved file", lost);
    for (int i = 0; i < 8; i++) { snprintf(nm, sizeof nm, "race_%d", i); path_of(p, sizeof p, nm); DeleteFileA(p); }
    DeleteFileA(dst);
}

static void test_copy(void) {
    char a[600], b[600];
    SECTION("CopyFileA: copies; fail_if_exists keeps the target; a folder as the source fails (ERROR_ACCESS_DENIED) and leaves the target alone");
    path_of(a, sizeof a, "c_src"); path_of(b, sizeof b, "c_dst");
    CHECK(put_file("c_src", "source bytes"));
    CHECK(CopyFileA(a, b, TRUE) && file_is("c_dst", "source bytes"));
    CHECK(put_file("c_src", "newer bytes"));
    SetLastError(0);
    CHECKF(!CopyFileA(a, b, TRUE) && GetLastError() == ERROR_ALREADY_EXISTS && file_is("c_dst", "source bytes"), "error %u", GetLastError());
    CHECK(CopyFileA(a, b, FALSE) && file_is("c_dst", "newer bytes"));
    DeleteFileA(b);
    path_of(a, sizeof a, "c_folder");
    CHECK(CreateDirectoryA(a, NULL));
    SetLastError(0);
    CHECKF(!CopyFileA(a, b, TRUE) && GetLastError() == ERROR_ACCESS_DENIED, "a folder copied as a file (error %u)", GetLastError());
    CHECK(!exists("c_dst"));                                          /* no 0-byte file left behind */
    CHECK(put_file("c_dst", "kept"));                                 /* nor is an existing target touched (Windows fails before it) */
    SetLastError(0);
    CHECKF(!CopyFileA(a, b, FALSE) && GetLastError() == ERROR_ACCESS_DENIED, "error %u", GetLastError());
    CHECK(file_is("c_dst", "kept"));
    DeleteFileA(b);
    path_of(a, sizeof a, "c_missing");
    CHECK(!CopyFileA(a, b, FALSE) && GetLastError() == ERROR_FILE_NOT_FOUND && !exists("c_dst"));
    path_of(a, sizeof a, "c_folder"); RemoveDirectoryA(a);
    path_of(a, sizeof a, "c_src"); DeleteFileA(a);
}

/* file views from 8 threads: before the view table was locked, two threads took one slot and the second view could not be
 * unmapped any more (each a leaked view); a torn slot could unmap another thread's view */
static HANDLE g_map;
static DWORD WINAPI view_churn(LPVOID arg) {
    int id = (int)(intptr_t)arg, bad = 0;
    for (int i = 0; i < 20000; i++) {
        size_t off = (size_t)((i + id) & 3) * 0x10000;
        const uint8_t *v = MapViewOfFile(g_map, FILE_MAP_READ, 0, (DWORD)off, 0x10000);
        if (!v) { bad++; continue; }
        if (v[0x1234] != (uint8_t)((off + 0x1234) * 7)) bad++;                  /* the view shows its own part of the file */
        if (!UnmapViewOfFile(v)) bad++;
    }
    return (DWORD)bad;
}
static void test_views(void) {
    SECTION("8 threads mapping and unmapping file views (160,000 pairs): every view unmaps, none shows another's bytes");
    char path[600];
    path_of(path, sizeof path, "views.bin");
    FILE *f = fopen(plat_path(path, path, sizeof path), "wb");
    CHECK(f != NULL);
    if (!f) return;
    for (size_t i = 0; i < 0x40000; i++) fputc((int)(uint8_t)(i * 7), f);
    fclose(f);
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_map = fh != INVALID_HANDLE_VALUE ? CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    CHECK(g_map != NULL);
    if (g_map) {
        HANDLE h[8];
        for (int i = 0; i < 8; i++) h[i] = CreateThread(NULL, 0, view_churn, (void *)(intptr_t)i, 0, NULL);
        CHECK(WaitForMultipleObjects(8, h, TRUE, 120000) == WAIT_OBJECT_0);
        for (int i = 0; i < 8; i++) { DWORD c; GetExitCodeThread(h[i], &c); CHECKF(c == 0, "thread %d: %u failed map/unmap pairs", i, c); CloseHandle(h[i]); }
        SECTION("UnmapViewOfFile of something that is not a view: ERROR_INVALID_ADDRESS; a full table refuses the next map");
        CHECK(!UnmapViewOfFile((void *)(uintptr_t)0x12340000) && GetLastError() == ERROR_INVALID_ADDRESS);
        const void *v[300];
        int got = 0;
        while (got < 300 && (v[got] = MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, 0x1000))) got++;
        CHECKF(got > 16 && got < 300 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY, "%d views, then error %u", got, GetLastError());
        int unmapped = 0;
        for (int i = 0; i < got; i++) unmapped += UnmapViewOfFile(v[i]) != 0;
        CHECK(unmapped == got);
        CloseHandle(g_map);
    }
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    DeleteFileA(path);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/ssbu_files_%d", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    if (!CreateDirectoryA(g_dir, NULL)) { printf("cannot make %s\n", g_dir); return 1; }
    test_find();
    test_move();
    test_move_race();
    test_copy();
    test_views();
    remove_tree(g_dir);
    return tp_done("test_files");
}
