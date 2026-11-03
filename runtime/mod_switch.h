/* The mods' switches, as the settings menu and the game's start see them (runtime/hle_mod_switch.c):
 * one setting for every mod, the mod manager's own mods/mods.json ("mods": {"<folder>": {"enabled":
 * true|false, "title": ...}}, "order": [...]), which tools/mods.py, tools/os_mods.py, the Mods app
 * and the launcher read and write too; plus its "code_patches" (true|false; absent: on while a mod
 * runs), the switch of ARCropolis' code patches (tools/recomp.py CODE_PATCHES, rt_code_patches_on).
 *
 * The menu changes them through tools/os_mods.py ("switch <folder> on|off", ":code_patches" for the
 * code patches), which writes mods.json and asks for an apply at the next start
 * (dump/mods/apply_pending): the game then applies the mods (tools/os_mods.py apply) before it
 * reads them. Nothing changes in a running game.
 *
 * The reading is here, with no file or process, for tools/dev/modswitchtest.c. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_SWITCH_CODE ":code_patches"            /* (os_mods.py switch's name for the code patches) */
#define MOD_SWITCH_CODE_TITLE "ARCropolis code patches"

typedef struct ModSwitch {
    char name[160];    /* its folder in mods/ (UTF-8) */
    char title[64];    /* the mod's title, else its folder */
    int on;
} ModSwitch;

/* ---- a little JSON: strings (UTF-8 out; \uXXXX and surrogate pairs), values skipped ---- */
static const char *msj_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static unsigned msj_hex4(const char *p) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0xFFFD;
        v = v * 16 + (unsigned)d;
    }
    return v;
}
/* A string at p (its opening quote) into out (n bytes, cut short if need be); the text after it,
 * or NULL when it is not one. */
static const char *msj_string(const char *p, char *out, size_t n) {
    size_t k = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (!*p) return NULL;
            if (*p == 'u') {
                if (strnlen(p + 1, 4) < 4) return NULL;
                c = msj_hex4(p + 1);
                p += 4;
                if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u' && strnlen(p + 3, 4) == 4) {
                    unsigned lo = msj_hex4(p + 3);
                    if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
            } else c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : (unsigned char)*p;
        }
        unsigned char e[4];
        int l = 0;
        if (c < 0x80) e[l++] = (unsigned char)c;
        else if (c < 0x800) { e[l++] = (unsigned char)(0xC0 | c >> 6); e[l++] = (unsigned char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { e[l++] = (unsigned char)(0xE0 | c >> 12); e[l++] = (unsigned char)(0x80 | (c >> 6 & 0x3F)); e[l++] = (unsigned char)(0x80 | (c & 0x3F)); }
        else { e[l++] = (unsigned char)(0xF0 | c >> 18); e[l++] = (unsigned char)(0x80 | (c >> 12 & 0x3F)); e[l++] = (unsigned char)(0x80 | (c >> 6 & 0x3F)); e[l++] = (unsigned char)(0x80 | (c & 0x3F)); }
        if (k + (size_t)l < n) { memcpy(out + k, e, (size_t)l); k += (size_t)l; }
    }
    if (n) out[k < n ? k : n - 1] = 0;
    return *p == '"' ? p + 1 : NULL;
}
/* Past the value at p (any kind); NULL when it is not one. */
static const char *msj_skip(const char *p, int depth) {
    p = msj_ws(p);
    if (depth > 64) return NULL;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = msj_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                char key[8];
                if (!(p = msj_string(msj_ws(p), key, sizeof key))) return NULL;
                p = msj_ws(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = msj_skip(p, depth + 1))) return NULL;
            p = msj_ws(p);
            if (*p == ',') { p = msj_ws(p + 1); continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (!*p) return NULL;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
    return p;
}

/* mods.json's text into out (max at most), in its "order" (a mod not in it after, in the file's
 * order): how many; *code_patches its "code_patches" (1, 0, or -1 when absent). -1 when the text
 * is not a JSON object. */
static int mod_switch_read(const char *js, ModSwitch *out, int max, int *code_patches) {
    int n = 0;
    *code_patches = -1;
    const char *p = msj_ws(js), *mods = NULL, *order = NULL;
    if (*p != '{') return -1;
    p = msj_ws(p + 1);
    while (*p && *p != '}') {
        char key[32];
        if (!(p = msj_string(p, key, sizeof key))) return -1;
        p = msj_ws(p);
        if (*p++ != ':') return -1;
        p = msj_ws(p);
        if (!strcmp(key, "mods") && *p == '{') mods = p;
        else if (!strcmp(key, "order") && *p == '[') order = p;
        else if (!strcmp(key, "code_patches")) *code_patches = !strncmp(p, "true", 4) ? 1 : !strncmp(p, "false", 5) ? 0 : -1;
        if (!(p = msj_skip(p, 0))) return -1;
        p = msj_ws(p);
        if (*p == ',') p = msj_ws(p + 1);
    }
    if (!mods) return 0;
    /* every mod of "mods": its folder, title and switch */
    p = msj_ws(mods + 1);
    while (*p && *p != '}' && n < max) {
        ModSwitch *m = &out[n];
        memset(m, 0, sizeof *m);
        if (!(p = msj_string(p, m->name, sizeof m->name))) return -1;
        p = msj_ws(p);
        if (*p++ != ':') return -1;
        p = msj_ws(p);
        if (*p == '{') {
            const char *q = msj_ws(p + 1);
            while (*q && *q != '}') {
                char key[32];
                if (!(q = msj_string(q, key, sizeof key))) return -1;
                q = msj_ws(q);
                if (*q++ != ':') return -1;
                q = msj_ws(q);
                if (!strcmp(key, "enabled")) m->on = !strncmp(q, "true", 4);
                else if (!strcmp(key, "title") && *q == '"') msj_string(q, m->title, sizeof m->title);
                if (!(q = msj_skip(q, 1))) return -1;
                q = msj_ws(q);
                if (*q == ',') q = msj_ws(q + 1);
            }
        }
        if (!m->title[0]) snprintf(m->title, sizeof m->title, "%s", m->name);
        if (!(p = msj_skip(p, 0))) return -1;
        p = msj_ws(p);
        if (*p == ',') p = msj_ws(p + 1);
        n++;
    }
    /* in "order"'s order */
    if (order && n > 1) {
        int placed = 0;
        p = msj_ws(order + 1);
        while (*p && *p != ']' && placed < n) {
            char name[160];
            if (!(p = msj_string(p, name, sizeof name))) break;
            for (int i = placed; i < n; i++)
                if (!strcmp(out[i].name, name)) {
                    ModSwitch t = out[i];
                    memmove(out + placed + 1, out + placed, sizeof *out * (size_t)(i - placed));
                    out[placed++] = t;
                    break;
                }
            p = msj_ws(p);
            if (*p == ',') p = msj_ws(p + 1);
        }
    }
    return n;
}

/* The code patches' switch from mods.json's (1, 0, -1 absent) and whether a mod runs now (the
 * applied list, dump/skyline/mods.txt, has one): absent, they are on while a mod runs, so a game
 * with no mod runs the game's own code. */
static int mod_switch_code_patches(int setting, int a_mod_runs) { return setting >= 0 ? setting : a_mod_runs != 0; }

/* Whether a folder name can be handed to os_mods.py on a command line (quoted). */
static int mod_switch_name_ok(const char *name) {
    if (!name[0] || strlen(name) > 150) return 0;
    for (const char *s = name; *s; s++) if (*s == '"' || (unsigned char)*s < 0x20 || *s == '%' || *s == '\\') return 0;
    return 1;
}

/* ---- the game's side (hle_mod_switch.c) ---- */
void mods_start(const char *data_dir);                           /* at start: a pending apply, then the code patches' switch */
int mods_switch_list(ModSwitch *out, int max, int *code_patches); /* mods.json now; -1 when unreadable */
int mods_switch_set(const char *name, int on);                   /* MOD_SWITCH_CODE for the code patches; 0 when it can't start */
enum { MODS_SWITCH_IDLE, MODS_SWITCH_WORKING, MODS_SWITCH_DONE, MODS_SWITCH_FAILED };
int mods_switch_state(char *msg, size_t n);                      /* the last change's: MODS_SWITCH_* and a line */
int mods_running_count(void);                                    /* mods the game runs now (dump/skyline/mods.txt at start) */
