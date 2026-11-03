/* What differs between two games online, in words (runtime/net_relay.c): the room browser's "why
 * not" and its room details, and a relay's refusal (REJECT's detail). The mods are mod sets' entries
 * (net_relay.c mod_set_make: a name and a hash each; the hash tells two copies apart); ARCropolis'
 * code patches are one entry of their own when they are on (MOD_CODE_PATCHES), said as such.
 *
 * Here with no file or socket, for tools/dev/moddifftest.c. */
#pragma once
#include <stdio.h>
#include <string.h>

#define MOD_NAME 31
#define MOD_CODE_PATCHES "ARCropolis code patches"   /* (the entry's name: mod_switch.h MOD_SWITCH_CODE_TITLE) */
typedef struct Mod { char name[MOD_NAME + 1]; unsigned hash; } Mod;

static int mod_name_eq(const char *a, const char *b) {   /* (case not counted, ASCII) */
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}
static int mod_find(const Mod *m, int n, const char *name) {
    for (int i = 0; i < n; i++) if (mod_name_eq(m[i].name, name)) return i;
    return -1;
}
static void mod_names_add(char *out, size_t n, const char *what) {   /* (", " between) */
    size_t t = strlen(out);
    if (t + 1 < n) snprintf(out + t, n - t, "%s%s", t ? ", " : "", what);
}
static void mod_line_add(char *out, size_t n, const char *head, const char *what, const char *tail) {   /* (a line each; none for "") */
    size_t t = strlen(out);
    if (what[0] && t + 1 < n) snprintf(out + t, n - t, "%s%s%s%s", t ? "\n" : "", head, what, tail);
}

/* The mods of theirs (a room's, a relay's room's host's) against mine, a line each: the code patches
 * on one side only, the mods only they have, the ones both have in other copies, the ones only I
 * have. where: "this room", "the room". "" when the sets are the same. */
static void mods_diff_text(const Mod *theirs, int nt, const Mod *mine, int nm, const char *where, char *out, size_t n) {
    char missing[256] = "", other[256] = "", extra[256] = "";
    out[0] = 0;
    int code_t = mod_find(theirs, nt, MOD_CODE_PATCHES) >= 0, code_m = mod_find(mine, nm, MOD_CODE_PATCHES) >= 0;
    if (code_t != code_m) {
        size_t t = strlen(out);
        snprintf(out + t, n - t, "%sCode patches (ARCropolis): %s in %s, %s in yours (Mods page)", t ? "\n" : "", code_t ? "on" : "off", where,
                 code_m ? "on" : "off");
    }
    for (int i = 0; i < nt; i++) {
        if (mod_name_eq(theirs[i].name, MOD_CODE_PATCHES)) continue;
        int k = mod_find(mine, nm, theirs[i].name);
        if (k < 0) mod_names_add(missing, sizeof missing, theirs[i].name);
        else if (mine[k].hash != theirs[i].hash) mod_names_add(other, sizeof other, theirs[i].name);
    }
    for (int k = 0; k < nm; k++)
        if (!mod_name_eq(mine[k].name, MOD_CODE_PATCHES) && mod_find(theirs, nt, mine[k].name) < 0) mod_names_add(extra, sizeof extra, mine[k].name);
    char head[64];
    snprintf(head, sizeof head, "Mods not in %s: ", where);
    mod_line_add(out, n, "Mods you don't have: ", missing, "");
    mod_line_add(out, n, "Another copy of: ", other, " (yours differs from the room's)");
    mod_line_add(out, n, head, extra, " (turn them off to join)");
}

/* A room's mods in one line, each marked as I have it: "A, B (you don't have it), C (yours differs)". */
static void mods_list_marked(const Mod *theirs, int nt, const Mod *mine, int nm, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < nt; i++) {
        char one[MOD_NAME + 32];
        int k = mod_find(mine, nm, theirs[i].name);
        int code = mod_name_eq(theirs[i].name, MOD_CODE_PATCHES);
        snprintf(one, sizeof one, "%s%s", theirs[i].name, k < 0 ? (code ? " (off in yours)" : " (you don't have it)")
                                                         : mine[k].hash != theirs[i].hash ? " (yours differs)" : "");
        mod_names_add(out, n, one);
    }
}
