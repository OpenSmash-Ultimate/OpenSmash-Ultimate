/* The settings files' folder (config_folder.h): config/, its version.ini, and the copy of the old files
 * into it on the first start that has it. (Not an HLE of anything: named hle_ so both builds'
 * runtime file patterns take it.)
 *
 * config/version.ini, written last, after every copy:
 *   version = 1
 *   [migrated]
 *   windowsmash = display.ini, controllers.ini      what came from the port's folder
 *   ssbu-recomp = build\config: graphics.ini, ...   what came from an ssbu-recomp install
 * Until it is there the copy is tried again at each start; once it is, never. A start that cannot
 * make or write the folder takes the old layout (the files in the port's folder) and removes what
 * it had copied, so the next start copies the files as they are then.
 *
 * WindowSmash kept display.ini, controllers.ini, launcher.ini, online_name.txt and
 * online_direct.txt in the port's folder: each is copied as it is (and online_protocol.txt, newer
 * than the folder, if one is there). online_server.txt is the port's own file there (the
 * installers update it, keeping a lobby code added to it): it is read there until config/ has
 * one of its own.
 *
 * ssbu-recomp kept its settings beside its program, in <build folder>/config: build/ in its folder
 * (build-linux/, build-macos/), or the folder its build_location.txt names; or the same in this
 * folder's ssbu-recomp/ (the staged tree, built where it is). The first such folder found is
 * copied as it is into config/ssbu-recomp/ (never changed there), and what has an equivalent
 * here is carried over where config/ has no file of its own yet:
 *   controllers.ini   rewritten in this port's terms (hle_pad.c's pad_ini_convert)
 *   graphics.ini      window, width, height, vsync, gpu, resolution_scale, profile and
 *                     stats_overlay into display.ini; player_name into online_name.txt;
 *                     online_direct = off into online_direct.txt. Its lobby is not: ssbu-recomp's
 *                     online play talks another protocol.
 * Its other keys (the renderer's, the online code's) stay in config/ssbu-recomp/graphics.ini. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "config_folder.h"
#include "settings.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif
#define FOLDER "config"

static const char *const NAMES[CONFIG_FILES] = {"display.ini", "controllers.ini", "launcher.ini", "online_server.txt",
                                                "online_name.txt", "online_direct.txt", "online_protocol.txt"};
static char paths[CONFIG_FILES][64];

/* ---- files ------------------------------------------------------------------------------- */
static int is_file(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_dir(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* A whole file, NUL-terminated (NULL: not readable). */
static char *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    int bad = ferror(f);
    fclose(f);
    if (!buf || bad) { free(buf); return NULL; }
    buf[got] = 0;
    if (size) *size = got;
    return buf;
}

/* What this start made, taken back if it cannot finish (files, then folders, newest first). */
static struct { char path[300]; int dir; } made[64];
static int nmade;

static void note_made(const char *path, int dir) {
    if (nmade < (int)(sizeof made / sizeof *made)) { snprintf(made[nmade].path, sizeof made[nmade].path, "%s", path); made[nmade++].dir = dir; }
}

static void take_back(void) {
    while (nmade > 0) {
        nmade--;
        if (made[nmade].dir) RemoveDirectoryA(made[nmade].path);   /* (only when empty) */
        else DeleteFileA(made[nmade].path);
    }
}

static int make_dir(const char *path) {
    if (is_dir(path)) return 1;
    if (!CreateDirectoryA(path, NULL)) return 0;
    note_made(path, 1);
    return 1;
}

/* A file written where there was none. */
static int write_new(const char *path, const void *data, size_t size) {
    if (!settings_file_write(path, data, size)) return 0;
    note_made(path, 0);
    return 1;
}

/* The same with text of the program's own, in this system's line ends (as the game writes its
 * files' defaults). */
static int write_new_text(const char *path, const char *text) {
#ifdef _WIN32
    size_t n = 0;
    char *crlf = malloc(2 * strlen(text) + 1);
    if (!crlf) return 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && (p == text || p[-1] != '\r')) crlf[n++] = '\r';
        crlf[n++] = *p;
    }
    int ok = write_new(path, crlf, n);
    free(crlf);
    return ok;
#else
    return write_new(path, text, strlen(text));
#endif
}

/* from copied to `to` as it is, when `to` is not there yet. 1 copied, 0 not (nothing to copy, or
 * `to` there already), -1 failed. */
static int copy_new(const char *from, const char *to) {
    if (!is_file(from) || is_file(to)) return 0;
    size_t size = 0;
    char *data = read_all(from, &size);
    int ok = data && write_new(to, data, size);
    free(data);
    if (!ok) fprintf(stderr, "[config] cannot copy %s to %s\n", from, to);
    return ok ? 1 : -1;
}

/* ---- ssbu-recomp ------------------------------------------------------------------------- */
static int sr_folder(const char *dir) {
    char p[400];
    snprintf(p, sizeof p, "%s" SEP "controllers.ini", dir);
    if (is_file(p)) return 1;
    snprintf(p, sizeof p, "%s" SEP "graphics.ini", dir);
    return is_file(p);
}

/* The config folder of an ssbu-recomp install in this folder: into out, or 0. */
static int find_sr(char *out, size_t size) {
    static const char *const TOPS[] = {"", "ssbu-recomp" SEP};
    static const char *const BUILDS[] = {"build", "build-linux", "build-macos"};
    for (int t = 0; t < 2; t++) {
        char p[400];
        snprintf(p, sizeof p, "%sbuild_location.txt", TOPS[t]);
        char *line = read_all(p, NULL);   /* (its build folder, chosen in its installer) */
        if (line) {
            char *s = line + (!strncmp(line, "\xEF\xBB\xBF", 3) ? 3 : 0), *e;
            s[strcspn(s, "\r\n")] = 0;
            while (*s == ' ' || *s == '\t' || *s == '"') s++;
            for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"' || e[-1] == '\\' || e[-1] == '/'); ) *--e = 0;
            snprintf(out, size, "%s" SEP "config", s);
            int found = *s && sr_folder(out);
            free(line);
            if (found) return 1;
        }
        for (int b = 0; b < 3; b++) {
            snprintf(out, size, "%s%s" SEP "config", TOPS[t], BUILDS[b]);
            if (sr_folder(out)) return 1;
        }
    }
    out[0] = 0;
    return 0;
}

static int word_is(const char *v, const char *const *words) {
    for (; *words; words++) if (!_stricmp(v, *words)) return 1;
    return 0;
}

static const char *const ON[] = {"on", "1", "yes", "true", NULL}, *const OFF[] = {"off", "0", "no", "false", NULL};

/* graphics.ini's keys that display.ini has, into display.ini (made with its defaults first). */
static int display_from_sr(const char *graphics, const char *display) {
    char v[160], tmp[300], scale[32] = "";
    snprintf(tmp, sizeof tmp, "%s.new", display);
    if (!write_new_text(tmp, display_default_ini())) return 0;
    int ok = 1, n = 0;
#define PUT(key, value) do { ok = ok && settings_ini_set(tmp, NULL, key, value); n++; } while (0)
    if (settings_ini_get(graphics, NULL, "window", v, sizeof v) && word_is(v, (const char *const[]){"windowed", "borderless", "fullscreen", NULL}))
        PUT("window", v);
    if (settings_ini_get(graphics, NULL, "width", v, sizeof v) && atoi(v) >= 320 && atoi(v) <= 16384) PUT("width", v);
    if (settings_ini_get(graphics, NULL, "height", v, sizeof v) && atoi(v) >= 180 && atoi(v) <= 16384) PUT("height", v);
    /* vsync: auto and display follow 60 / 120 / 240 Hz displays (else the clock), as pacing = display
     * does; off shows each frame at once, tearing, as vrr does without VRR */
    const char *pacing = NULL;
    if (settings_ini_get(graphics, NULL, "vsync", v, sizeof v))
        pacing = word_is(v, (const char *const[]){"auto", "display", NULL}) ? "display" : !_stricmp(v, "off") ? "vrr" :
                 word_is(v, (const char *const[]){"clock", "on", "mailbox", NULL}) ? "clock" : NULL;
    if (settings_ini_get(graphics, NULL, "gpu", v, sizeof v) && _stricmp(v, "auto") && strspn(v, "0123456789") != strlen(v))
        PUT("gpu", v);   /* (a device's number in ssbu-recomp's list means nothing here) */
    if (settings_ini_get(graphics, NULL, "resolution_scale", v, sizeof v) && atof(v) > 0) {
        double x = atof(v);
        snprintf(scale, sizeof scale, "%g", x < 0.5 ? 0.5 : x > 4 ? 4 : x);
    }
    if (settings_ini_get(graphics, NULL, "profile", v, sizeof v) && !_stricmp(v, "low")) {   /* (it won over those keys there) */
        snprintf(scale, sizeof scale, "0.5");
        pacing = "clock";
    }
    if (pacing) PUT("pacing", pacing);
    if (scale[0]) PUT("render_scale", scale);
    if (settings_ini_get(graphics, NULL, "stats_overlay", v, sizeof v) && (word_is(v, ON) || word_is(v, OFF)))
        PUT("overlay", word_is(v, ON) ? "1" : "0");
#undef PUT
    if (ok && (ok = MoveFileExA(tmp, display, MOVEFILE_WRITE_THROUGH)) != 0)
        snprintf(made[nmade - 1].path, sizeof made[nmade - 1].path, "%s", display);   /* (tmp's entry: the file made is display.ini now) */
    if (!ok) fprintf(stderr, "[config] cannot write %s\n", display);
    else fprintf(stderr, "[config] %s: %d setting%s of %s\n", display, n, n == 1 ? "" : "s", graphics);
    return ok;
}

/* ---- the folder ------------------------------------------------------------------------ */
static void set_paths(const char *dir) {
    for (int f = 0; f < CONFIG_FILES; f++)
        if (!strcmp(dir, ".")) snprintf(paths[f], sizeof paths[f], "%s", NAMES[f]);
        else snprintf(paths[f], sizeof paths[f], "%s" SEP "%s", dir, NAMES[f]);
}

static void list_add(char *list, size_t size, const char *name) {
    size_t n = strlen(list);
    snprintf(list + n, size - n, "%s%s", n ? ", " : "", name);
}

static const char VERSION_HEAD[] =
    "; version.ini: the layout of this folder, which holds the game's settings files.\n"
    "; The game wrote it the first time it started with this folder, after copying the settings files\n"
    "; in from where they were before (they are still there, as they were): [migrated] says which.\n"
    "; Delete it to have them copied again (a file already here is kept).\n";

/* 1: the folder is ready; 0: the old layout for this start. */
static int migrate(void) {
    char version[300], v[32];
    snprintf(version, sizeof version, FOLDER SEP "version.ini");
    if (is_file(version)) {
        int n = settings_ini_get(version, NULL, "version", v, sizeof v) ? atoi(v) : CONFIG_VERSION;
        if (n > CONFIG_VERSION)
            fprintf(stderr, "[config] %s is version %d, newer than this program's %d: its files are used as they are\n", version, n, CONFIG_VERSION);
        return 1;
    }
    if (!make_dir(FOLDER)) {
        fprintf(stderr, "[config] cannot make the folder " FOLDER " here (error %lu): the settings files stay where they were\n",
                (unsigned long)GetLastError());
        return 0;
    }
    char ws[200] = "", sr_list[200] = "", sr[300] = "", to[400], from[400];
    int ok = 1;
    for (int f = 0; f < CONFIG_FILES && ok; f++) {
        if (f == CONFIG_ONLINE_SERVER) continue;
        snprintf(to, sizeof to, FOLDER SEP "%s", NAMES[f]);
        int r = copy_new(NAMES[f], to);
        if (r > 0) list_add(ws, sizeof ws, NAMES[f]);
        ok = r >= 0;
    }
    if (ok && find_sr(sr, sizeof sr)) {
        static const char *const SR_FILES[] = {"controllers.ini", "graphics.ini"};
        ok = make_dir(FOLDER SEP "ssbu-recomp");
        for (int i = 0; i < 2 && ok; i++) {
            snprintf(from, sizeof from, "%s" SEP "%s", sr, SR_FILES[i]);
            snprintf(to, sizeof to, FOLDER SEP "ssbu-recomp" SEP "%s", SR_FILES[i]);
            int r = copy_new(from, to);
            if (r > 0 || (r == 0 && is_file(from))) list_add(sr_list, sizeof sr_list, SR_FILES[i]);
            ok = r >= 0;
        }
        const char *pads = FOLDER SEP "ssbu-recomp" SEP "controllers.ini", *graphics = FOLDER SEP "ssbu-recomp" SEP "graphics.ini";
        if (ok && is_file(pads)) {
            if (is_file(paths[CONFIG_CONTROLLERS]))
                fprintf(stderr, "[config] %s is the one in use; ssbu-recomp's is kept in %s\n", paths[CONFIG_CONTROLLERS], pads);
            else if ((ok = pad_ini_convert(pads, paths[CONFIG_CONTROLLERS])) != 0) note_made(paths[CONFIG_CONTROLLERS], 0);
        }
        if (ok && is_file(graphics)) {
            if (!is_file(paths[CONFIG_DISPLAY])) ok = display_from_sr(graphics, paths[CONFIG_DISPLAY]);
            if (ok && !is_file(paths[CONFIG_ONLINE_NAME]) && settings_ini_get(graphics, NULL, "player_name", v, sizeof v) && v[0]) {
                char name[40];
                snprintf(name, sizeof name, "%.32s", v);   /* (as long as the settings menu takes) */
                if ((ok = settings_line_write(paths[CONFIG_ONLINE_NAME], name)) != 0) note_made(paths[CONFIG_ONLINE_NAME], 0);
            }
            if (ok && !is_file(paths[CONFIG_ONLINE_DIRECT]) && settings_ini_get(graphics, NULL, "online_direct", v, sizeof v) && word_is(v, OFF))
                if ((ok = settings_line_write(paths[CONFIG_ONLINE_DIRECT], "off")) != 0) note_made(paths[CONFIG_ONLINE_DIRECT], 0);
        }
    }
    if (ok) {
        char text[2048];
        int n = snprintf(text, sizeof text, "%sversion = %d\n\n[migrated]\n", VERSION_HEAD, CONFIG_VERSION);
        if (ws[0]) n += snprintf(text + n, sizeof text - n, "windowsmash = %s\n", ws);
        if (sr[0]) n += snprintf(text + n, sizeof text - n, "ssbu-recomp = %s: %s\n", sr, sr_list);
        ok = write_new_text(version, text);
    }
    if (!ok) {
        take_back();
        fprintf(stderr, "[config] cannot write the folder " FOLDER ": the settings files stay where they were\n");
        return 0;
    }
    if (ws[0]) fprintf(stderr, "[config] copied %s into " FOLDER " (the old files stay where they were)\n", ws);
    if (sr[0]) fprintf(stderr, "[config] ssbu-recomp's settings in %s: %s, kept in " FOLDER SEP "ssbu-recomp\n", sr, sr_list);
    nmade = 0;
    return 1;
}

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    set_paths(FOLDER);   /* (where migrate looks and writes) */
    if (!migrate()) set_paths(".");
    return TRUE;
}

const char *config_path(enum ConfigFile file) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_once, NULL, NULL);
    if ((unsigned)file >= CONFIG_FILES) return NULL;
    if (file == CONFIG_ONLINE_SERVER && !is_file(paths[file])) return NAMES[file];   /* (the port's own) */
    return paths[file];
}
