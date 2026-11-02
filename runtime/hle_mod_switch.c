/* The mods' switches (mod_switch.h): the game's start (a pending apply, the code patches' switch,
 * rt_code_patches_on) and the settings menu's Mods page (each mod on or off through
 * tools/os_mods.py, which writes mods/mods.json). (Not an HLE of anything: named hle_ so every
 * build's runtime file pattern takes it.)
 *
 * The paths are the port's: <root>/mods/mods.json, <root>/dump/skyline/mods.txt (the mods the
 * last apply turned on, which ARCropolis' mods: mount reads, hle_fs.c), <root>/dump/mods/apply_pending
 * (os_mods.py switch's: apply at the next start), with <root> two folders above the data folder
 * (dump/modules). The Python is the one the launchers find (tools/find_python.bat): deps/python,
 * else python on the PATH (python3 on Linux and macOS, or deps/venv's). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include "host.h"
#include "mod_switch.h"

static char root[1024];          /* the port's folder (no slash at its end) */
static int code_patches = -1;    /* rt_code_patches_on's, from mods_start */
static int running = -1;         /* mods the game runs (dump/skyline/mods.txt) */

static void root_from(const char *data_dir) {
    char rel[1100];
    snprintf(rel, sizeof rel, "%s/../..", data_dir && *data_dir ? data_dir : "dump/modules");
#ifdef _WIN32
    if (!GetFullPathNameA(rel, sizeof root, root, NULL)) snprintf(root, sizeof root, "%s", rel);
#else
    if (!realpath(rel, root)) snprintf(root, sizeof root, "%s", rel);
#endif
    size_t n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\')) root[--n] = 0;
}

static char *read_all(const char *path, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *t = malloc(limit + 1);
    size_t n = t ? fread(t, 1, limit, f) : 0;
    fclose(f);
    if (t) t[n] = 0;
    return t;
}

static int exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* ---- running tools/os_mods.py ---- */
/* The command line (Windows) or argv (else) of os_mods.py with these arguments. */
#ifdef _WIN32
static void python_command(char *out, size_t n, const char *args) {
    char py[1100];
    snprintf(py, sizeof py, "%s\\deps\\python\\python.exe", root);
    if (exists(py)) snprintf(out, n, "\"%s\" \"%s\\tools\\os_mods.py\" %s", py, root, args);
    else snprintf(out, n, "python \"%s\\tools\\os_mods.py\" %s", root, args);
}
/* Runs it and waits (up to wait_ms); its exit code, or -1 when it didn't start or finish. Its output
 * goes to the game's log. */
static int run_python(const char *args, DWORD wait_ms) {
    char cmd[2400];
    python_command(cmd, sizeof cmd, args);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);   /* (its lines in the log, among the game's) */
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, root, &si, &pi)) {
        fprintf(stderr, "[mods] could not run %s (error %lu)\n", cmd, GetLastError());
        return -1;
    }
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#else
static int run_python(const char *args, DWORD wait_ms) {
    (void)wait_ms;   /* (waited for: an apply ends) */
    char py[1100], script[1100], a[4][200];
    const char *argv[8];
    int argc = 0, k = 0;
    snprintf(py, sizeof py, "%s/deps/venv/bin/python3", root);
    if (!exists(py)) snprintf(py, sizeof py, "%s/deps/python/bin/python3", root);
    if (!exists(py)) snprintf(py, sizeof py, "python3");
    snprintf(script, sizeof script, "%s/tools/os_mods.py", root);
    argv[argc++] = py;
    argv[argc++] = script;
    /* args: words, a quoted one as one (mod_switch_name_ok's names have no quote inside) */
    for (const char *s = args; *s && k < 4;) {
        while (*s == ' ') s++;
        if (!*s) break;
        size_t l = 0;
        if (*s == '"') { for (s++; *s && *s != '"' && l + 1 < sizeof a[k]; s++) a[k][l++] = *s; if (*s == '"') s++; }
        else for (; *s && *s != ' ' && l + 1 < sizeof a[k]; s++) a[k][l++] = *s;
        a[k][l] = 0;
        argv[argc++] = a[k++];
    }
    argv[argc] = NULL;
    pid_t pid;
    if (posix_spawnp(&pid, py, NULL, NULL, (char *const *)argv, environ)) {
        fprintf(stderr, "[mods] could not run %s %s\n", py, script);
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
#endif

/* ---- the game's start ---- */
void mods_start(const char *data_dir) {
    root_from(data_dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/dump/mods/apply_pending", root);
    if (exists(path)) {   /* (the settings menu turned a mod on or off: applied before the game reads them) */
        fprintf(stderr, "[mods] the mods changed in the settings menu: applying them\n");
        int code = run_python("apply", 15 * 60 * 1000);
        if (code == 0) remove(path);
        else fprintf(stderr, "[mods] the apply did not finish (code %d): the mods are as they were\n", code);
    }
    snprintf(path, sizeof path, "%s/dump/skyline/mods.txt", root);
    char *t = read_all(path, 1 << 20);
    running = 0;
    for (char *s = t; s && *s;) {
        char *e = s + strcspn(s, "\r\n");
        if (e > s) running++;
        s = *e ? e + 1 : e;
    }
    free(t);
    ModSwitch one;
    int setting = -1;
    snprintf(path, sizeof path, "%s/mods/mods.json", root);
    if ((t = read_all(path, 8 << 20))) {
        mod_switch_read(t, &one, 1, &setting);
        free(t);
    }
    code_patches = mod_switch_code_patches(setting, running);
    const char *env = getenv("SSBU_CODE_PATCHES");   /* (test runs) */
    if (env && (*env == '0' || *env == '1')) code_patches = *env == '1';
    fprintf(stderr, "[mods] %d mod%s run; %s: %s%s\n", running, running == 1 ? "" : "s", MOD_SWITCH_CODE_TITLE, code_patches ? "on" : "off",
            setting < 0 && !(env && (*env == '0' || *env == '1')) ? " (on while a mod runs)" : "");
}

int rt_code_patches_on(void) {
    if (code_patches < 0) {   /* (no mods_start: a test program) */
        const char *env = getenv("SSBU_CODE_PATCHES");
        code_patches = env && *env == '1';
    }
    return code_patches;
}

int mods_running_count(void) { return running < 0 ? 0 : running; }

/* ---- the settings menu ---- */
int mods_switch_list(ModSwitch *out, int max, int *setting) {
    char path[1200];
    snprintf(path, sizeof path, "%s/mods/mods.json", root[0] ? root : "../..");
    char *t = read_all(path, 8 << 20);
    *setting = -1;
    if (!t) return 0;   /* (no mods.json: no mod installed) */
    int n = mod_switch_read(t, out, max, setting);
    free(t);
    return n;
}

static SRWLOCK change_lock = SRWLOCK_INIT;
static volatile LONG change_state = MODS_SWITCH_IDLE;
static char change_msg[200], change_args[400];

static DWORD WINAPI change_thread(void *unused) {
    (void)unused;
    char args[400], msg[200];
    AcquireSRWLockExclusive(&change_lock);
    snprintf(args, sizeof args, "%s", change_args);
    ReleaseSRWLockExclusive(&change_lock);
    int code = run_python(args, 120000);
    snprintf(msg, sizeof msg, code == 0 ? "Changed: it counts when the game starts again (Restart now)"
                                        : "The change didn't go through (os_mods.py, code %d): see the log, or use the launcher", code);
    AcquireSRWLockExclusive(&change_lock);
    snprintf(change_msg, sizeof change_msg, "%s", msg);
    ReleaseSRWLockExclusive(&change_lock);
    InterlockedExchange(&change_state, code == 0 ? MODS_SWITCH_DONE : MODS_SWITCH_FAILED);
    return 0;
}

int mods_switch_set(const char *name, int on) {
    if (strcmp(name, MOD_SWITCH_CODE) && !mod_switch_name_ok(name)) return 0;
    if (change_state == MODS_SWITCH_WORKING) return 0;
    AcquireSRWLockExclusive(&change_lock);
    snprintf(change_args, sizeof change_args, "switch \"%s\" %s", name, on ? "on" : "off");
    snprintf(change_msg, sizeof change_msg, "Changing...");
    ReleaseSRWLockExclusive(&change_lock);
    InterlockedExchange(&change_state, MODS_SWITCH_WORKING);
    fprintf(stderr, "[mods] settings menu: %s %s\n", name, on ? "on" : "off");
    HANDLE t = CreateThread(NULL, 0, change_thread, NULL, 0, NULL);
    if (!t) { InterlockedExchange(&change_state, MODS_SWITCH_FAILED); return 0; }
    CloseHandle(t);
    return 1;
}

int mods_switch_state(char *msg, size_t n) {
    AcquireSRWLockShared(&change_lock);
    if (msg && n) snprintf(msg, n, "%s", change_msg);
    ReleaseSRWLockShared(&change_lock);
    return (int)change_state;
}
