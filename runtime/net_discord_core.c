/* Discord's sign-in and Rich Presence without the SDK (net_discord_core.h): the shared token file, the presence's text,
 * the join secret, the fighters' names and the player's position in the site's leaderboard.json. Plain C, no
 * Windows calls but the rename, so tools/dev/discordtest.c checks it on its own. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_discord_core.h"

#ifdef _WIN32
#include <windows.h>
#define REPLACE(from, to) MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)
#else
#define REPLACE(from, to) (rename(from, to) == 0)
#endif

/* ---- the token file ---------------------------------------------------------------------- */
static void put(char *dst, size_t cap, const char *v) {
    size_t n = strlen(v);
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

int discord_token_parse(const char *text, DiscordToken *t) {
    memset(t, 0, sizeof *t);
    if (!text) return 0;
    const char *p = text;
    while (*p) {
        const char *end = p + strcspn(p, "\r\n");
        char line[1024];
        size_t n = (size_t)(end - p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = *end ? end + 1 : end;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq) continue;
        *eq = 0;
        char *v = eq + 1, *k = s, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vn = strlen(v);
        while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) v[--vn] = 0;
        if (!strcmp(k, "access_token")) put(t->access, sizeof t->access, v);
        else if (!strcmp(k, "refresh_token")) put(t->refresh, sizeof t->refresh, v);
        else if (!strcmp(k, "scope")) put(t->scope, sizeof t->scope, v);
        else if (!strcmp(k, "expires_at")) t->expires_at = strtoll(v, NULL, 10);
        else if (!strcmp(k, "user_id")) put(t->user_id, sizeof t->user_id, v);
        else if (!strcmp(k, "username")) put(t->username, sizeof t->username, v);
        else if (!strcmp(k, "display_name")) put(t->display_name, sizeof t->display_name, v);
        else if (!strcmp(k, "avatar")) put(t->avatar, sizeof t->avatar, v);
    }
    return t->access[0] || t->refresh[0];
}

static int clean(const char *v) { return !strpbrk(v, "\r\n"); }   /* (a value with a line break would make another key) */

size_t discord_token_format(const DiscordToken *t, char *out, size_t cap) {
    const char *vals[] = {t->access, t->refresh, t->scope, t->user_id, t->username, t->display_name, t->avatar};
    for (size_t i = 0; i < sizeof vals / sizeof *vals; i++) if (!clean(vals[i])) return 0;
    int n = snprintf(out, cap,
                     "# OpenSmash: the Discord sign-in the game and the launchers share. Delete this file to sign out.\n"
                     "access_token=%s\nrefresh_token=%s\nexpires_at=%lld\nscope=%s\nuser_id=%s\nusername=%s\ndisplay_name=%s\navatar=%s\n",
                     t->access, t->refresh, (long long)t->expires_at, t->scope, t->user_id, t->username, t->display_name, t->avatar);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

int discord_token_load(const char *path, DiscordToken *t) {
    memset(t, 0, sizeof *t);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return discord_token_parse(buf, t);
}

int discord_token_save(const char *path, const DiscordToken *t) {
    char text[4096], tmp[1040];
    size_t n = discord_token_format(t, text, sizeof text);
    if (!n || strlen(path) + 5 > sizeof tmp) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(text, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok || !REPLACE(tmp, path)) { remove(tmp); return 0; }
    return 1;
}

int discord_token_fresh(const DiscordToken *t, int64_t now, int64_t margin) {
    return t->access[0] && t->expires_at > 0 && t->expires_at - margin > now;
}

/* ---- fighters ---------------------------------------------------------------------------- */
static const char *const FIGHTERS[] = {
    "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
    "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Sheik", "Zelda", "Dr. Mario", "Pichu",
    "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch", "Meta Knight",
    "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Squirtle", "Ivysaur", "Charizard", "Diddy Kong",
    "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man",
    "Wii Fit Trainer", "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt",
    "Ryu", "Ken", "Cloud", "Corrin", "Bayonetta", "Inkling", "Ridley", "Simon", "Richter", "King K. Rool",
    "Isabelle", "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Ice Climbers", NULL, NULL, NULL, NULL,
    NULL, "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth", "Min Min", "Steve", "Sephiroth",
    "Pyra", "Mythra", "Kazuya", "Sora"};   /* (by fighter kind: tools/paramconfig_kinds.py; 76-80: Nana, bosses, Mii enemies) */

const char *discord_fighter_name(int kind) {
    if (kind == 114) return "Pokemon Trainer";
    return kind >= 0 && kind < (int)(sizeof FIGHTERS / sizeof *FIGHTERS) ? FIGHTERS[kind] : NULL;
}

size_t discord_group_digits(uint32_t v, char *out, size_t cap) {
    char raw[16];
    int n = snprintf(raw, sizeof raw, "%u", v);
    size_t len = (size_t)n + (size_t)(n - 1) / 3;
    if (len + 1 > cap) { if (cap) out[0] = 0; return 0; }
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
    return o;
}

/* ---- presence ---------------------------------------------------------------------------- */
static const char *place_name(int where) {
    switch (where) {
    case DP_QUICKPLAY: return "Quickplay";
    case DP_ARENA: return "Battle Arena";
    case DP_ROOM: return "Local Wireless room";
    case DP_LOCAL: return "Smash";
    default: return "In the menus";
    }
}

void discord_presence_text(const DiscordPresence *p, char *details, size_t dn, char *state, size_t sn) {
    char d[160];
    snprintf(d, sizeof d, "%s", place_name(p->where));
    const char *fighter = p->where != DP_MENUS ? discord_fighter_name(p->fighter) : NULL;
    if (fighter) { strncat(d, " - ", sizeof d - strlen(d) - 1); strncat(d, fighter, sizeof d - strlen(d) - 1); }
    if (fighter && p->gsp) {
        char g[24];
        discord_group_digits(p->gsp, g, sizeof g);
        strncat(d, " - GSP ", sizeof d - strlen(d) - 1);
        strncat(d, g, sizeof d - strlen(d) - 1);
    }
    if (p->rank > 0) {
        char r[16];
        snprintf(r, sizeof r, " - #%d", p->rank);
        strncat(d, r, sizeof d - strlen(d) - 1);
    }
    snprintf(details, dn, "%s", d);
    switch (p->where) {
    case DP_QUICKPLAY: snprintf(state, sn, "%s", p->in_match ? "In a match" : "Looking for a match"); break;
    case DP_ARENA:
        if (p->target[0]) snprintf(state, sn, "Arena %s%s", p->target, p->in_match ? " - in a match" : "");
        else snprintf(state, sn, "%s", p->in_match ? "In a match" : "Looking for an arena");
        break;
    case DP_ROOM: snprintf(state, sn, "%s", p->in_match ? "In a match" : "Waiting in the room"); break;
    case DP_LOCAL: snprintf(state, sn, "%s", p->in_match ? "In a match" : ""); break;
    default: snprintf(state, sn, "%s", ""); break;
    }
}

static int secret_safe(const char *s) { return !strchr(s, '|'); }

size_t discord_join_secret(const DiscordPresence *p, char *out, size_t cap) {
    if (cap) out[0] = 0;
    char kind = p->where == DP_ARENA ? 'a' : p->where == DP_ROOM ? 'r' : 0;
    if (!kind || !p->target[0] || !secret_safe(p->target) || !secret_safe(p->code)) return 0;
    if (p->max_players > 0 && p->players >= p->max_players) return 0;
    int n = snprintf(out, cap, "os1|%c|%s|%s", kind, p->code, p->target);
    if (n <= 0 || (size_t)n >= cap || n > 128) { if (cap) out[0] = 0; return 0; }   /* (Discord: 128 characters at most) */
    return (size_t)n;
}

int discord_join_parse(const char *secret, DiscordJoin *j) {
    memset(j, 0, sizeof *j);
    if (!secret || strncmp(secret, "os1|", 4)) return 0;
    const char *k = secret + 4;
    if ((k[0] != 'a' && k[0] != 'r') || k[1] != '|') return 0;
    const char *code = k + 2, *bar = strchr(code, '|');
    if (!bar) return 0;
    const char *target = bar + 1;
    size_t cn = (size_t)(bar - code), tn = strlen(target);
    if (cn >= sizeof j->code || !tn || tn >= sizeof j->target || strchr(target, '|')) return 0;
    for (const char *c = target; *c; c++) if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) return 0;
    for (const char *c = code; c < bar; c++) if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) return 0;
    j->where = k[0] == 'a' ? DP_ARENA : DP_ROOM;
    memcpy(j->code, code, cn);
    memcpy(j->target, target, tn);
    return 1;
}

/* ---- leaderboard.json -------------------------------------------------------------------- */
/* Past a JSON string starting at p (p at the opening quote); its contents into out (escapes kept as they are). */
static const char *skip_string(const char *p, char *out, size_t cap) {
    size_t o = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) { if (out && o + 1 < cap) out[o++] = *p; p++; }
        if (out && o + 1 < cap) out[o++] = *p;
    }
    if (out && cap) out[o] = 0;
    return *p ? p + 1 : p;
}

/* One object of "players" (p at '{'): its top-level "rank", "puid" and "discord_id"; returns past its '}' (NULL: the
 * text ends first). */
static const char *read_player(const char *p, long *rank, char *puid, size_t pn, char *did, size_t dn) {
    int depth = 0;
    char key[32] = "";
    int want_value = 0;
    *rank = 0; puid[0] = did[0] = 0;
    for (; *p; ) {
        char c = *p;
        if (c == '"') {
            char s[128];
            const char *after = skip_string(p, s, sizeof s);
            if (depth == 1 && want_value) {
                if (!strcmp(key, "puid")) put(puid, pn, s);
                else if (!strcmp(key, "discord_id")) put(did, dn, s);
                want_value = 0;
            } else if (depth == 1) {
                const char *q = after;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                if (*q == ':') { put(key, sizeof key, s); want_value = 1; after = q + 1; }
            }
            p = after;
            continue;
        }
        if (c == '{' || c == '[') { depth++; if (depth > 1) want_value = 0; }
        else if (c == '}' || c == ']') { if (--depth == 0) return p + 1; }
        else if (depth == 1 && want_value && (c == '-' || (c >= '0' && c <= '9'))) {
            char *end;
            long v = strtol(p, &end, 10);
            if (!strcmp(key, "rank")) *rank = v;
            want_value = 0;
            p = end;
            continue;
        } else if (depth == 1 && want_value && c != ' ' && c != '\t' && c != '\r' && c != '\n') want_value = 0;   /* (null, true...) */
        p++;
    }
    return NULL;   /* (cut short: not a whole object) */
}

int discord_rank_in_leaderboard(const char *json, const char *puid, const char *discord_id) {
    if (!json) return 0;
    const char *p = strstr(json, "\"players\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    for (;;) {
        while (*p && *p != '{' && *p != ']') { if (*p == '"') p = skip_string(p, NULL, 0); else p++; }
        if (*p != '{') return 0;
        long rank;
        char pu[64], di[32];
        p = read_player(p, &rank, pu, sizeof pu, di, sizeof di);
        if (!p) return 0;
        if (rank > 0 && ((puid && *puid && !strcmp(pu, puid)) || (discord_id && *discord_id && !strcmp(di, discord_id))))
            return rank > 1000000 ? 0 : (int)rank;
    }
}
