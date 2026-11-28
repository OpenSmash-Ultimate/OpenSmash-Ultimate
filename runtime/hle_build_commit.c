/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The commit this program was built from (SSBU_BUILD_COMMIT_CHECK, on unless 0).
 *
 * The 40 '?' after the marker below are written over with the commit by the build, right after it links the program
 * (tools/build_commit.py stamp, from tools/recomp.py's link and tools/runtime_fetch.py), and the game library has one
 * of its own (tools/game_dll.py: rt_game_build_commit). At start the program says both, and the sources' commit
 * (tools/source_version.txt, which the updater writes), with a warning when the build is not the sources' one: an
 * update whose build was skipped or failed. Before, reports named the sources' commit only, which said nothing of
 * the program that ran (two PCs ran a build of 17/11 and reported v0.5.4's commit). The crash reports end with it too
 * (runtime/host.c report_close), and the launcher sends it with a report (OpenSmash.Core/BuildCommit.cs).
 * SSBU_BUILD_COMMIT_CHECK=0: nothing said, as before. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* (volatile: read from the file's bytes at run time, never folded by the compiler into the '?' it was compiled with) */
volatile const char rt_build_commit_mark[] = "SSBU-BUILD-COMMIT:????????????????????????????????????????";
#define MARK_HEAD 18   /* strlen("SSBU-BUILD-COMMIT:"), not written out: the marker is in the file only once */

static int is_commit(const char *s) {
    for (int i = 0; i < 40; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static void copy_mark(volatile const char *mark, char out[41]) {
    for (int i = 0; i < 40; i++) out[i] = mark[MARK_HEAD + i];
    out[40] = 0;
    if (!is_commit(out)) out[0] = 0;
}

/* The program's commit (40 hex digits), or NULL when the build did not write it: a report's last lines (host.c) */
const char *rt_build_commit(void) {
    static char commit[41];
    copy_mark(rt_build_commit_mark, commit);
    return commit[0] ? commit : NULL;
}

static int build_commit_on(void) {
    const char *v = getenv("SSBU_BUILD_COMMIT_CHECK");
    return !(v && v[0] == '0' && !v[1]);
}

/* The first word of a file, lower case, when it is a commit */
static int file_commit(const char *path, char out[41]) {
    FILE *f = fopen(path, "rb");
    char line[256] = {0};
    out[0] = 0;
    if (!f) return 0;
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    char *s = line;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    for (int i = 0; i < 40 && s[i]; i++) out[i] = (s[i] >= 'A' && s[i] <= 'F') ? (char)(s[i] - 'A' + 'a') : s[i];
    out[40] = 0;
    if (strlen(s) < 40 || !is_commit(out)) { out[0] = 0; return 0; }
    return 1;
}

/* The sources' commit: tools/source_version.txt (a player's copy), else git's HEAD (a checkout), in the port root
 * every launcher starts the game in */
static int source_commit(char out[41]) {
    if (file_commit("tools/source_version.txt", out)) return 1;
    char head[256] = {0};
    FILE *f = fopen(".git/HEAD", "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = 0;
    if (strncmp(head, "ref: ", 5)) return file_commit(".git/HEAD", out);
    char *ref = head + 5, path[300];
    ref[strcspn(ref, "\r\n")] = 0;
    snprintf(path, sizeof path, ".git/%s", ref);
    if (file_commit(path, out)) return 1;
    f = fopen(".git/packed-refs", "rb");   /* (a ref git packed: "<sha> <ref>" lines) */
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) > 41 && !strcmp(line + 41, ref)) {
            memcpy(out, line, 40);
            out[40] = 0;
            fclose(f);
            if (is_commit(out)) return 1;
            out[0] = 0;
            return 0;
        }
    }
    fclose(f);
    return 0;
}

/* Whether build_commit.txt beside the program lists commit as one this build is current for (tools/build_commit.py
 * reason: an update of only what is not built, the launcher's) */
static int listed_current(const char *commit) {
#ifdef _WIN32
    char path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    char *slash = strrchr(path, '\\');
    if (!slash) return 0;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "build_commit.txt");
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (!found && fgets(line, sizeof line, f))
        if (!strncmp(line, "current ", 8) && strstr(line, commit)) found = 1;
    fclose(f);
    return found;
#else
    (void)commit;
    return 0;
#endif
}

/* host.c, at start: which build this is, for reports */
void rt_build_commit_boot(void) {
    if (!build_commit_on()) return;
    char exe[41], lib[41] = {0}, src[41];
    copy_mark(rt_build_commit_mark, exe);
#ifdef _WIN32
    HMODULE game = GetModuleHandleA("ssbu_game.dll");
    volatile const char *mark = game ? (volatile const char *)(uintptr_t)GetProcAddress(game, "rt_game_build_commit") : NULL;
    if (mark) copy_mark(mark, lib);
#endif
    int have_src = source_commit(src);
    char library[80] = "";
#ifdef _WIN32
    if (game) snprintf(library, sizeof library, " (game library %.10s)", lib[0] ? lib : "not recorded");
#endif
    if (!exe[0]) {
        fprintf(stderr, "[boot] build commit: not recorded%s; the sources are at %.10s\n", library, have_src ? src : "an unknown commit");
        return;
    }
    if (!have_src) {
        fprintf(stderr, "[boot] build commit %s%s; the sources do not say their commit\n", exe, library);
        return;
    }
    if (!strcmp(exe, src)) {
        fprintf(stderr, "[boot] build commit %s%s, the sources' too\n", exe, library);
        return;
    }
    if (listed_current(src)) {
        fprintf(stderr, "[boot] build commit %s%s; the sources are at %.10s, with the same files to build\n", exe, library, src);
        return;
    }
    fprintf(stderr, "[boot] build commit %s%s\n", exe, library);
    fprintf(stderr, "[boot] WARNING: this build is commit %.10s but the sources are at %.10s: the game was not built again after "
                    "an update (the launcher's Update builds it; tools\\skyline.py uptodate says why)\n", exe, src);
}
