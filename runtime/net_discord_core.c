/* Discord's sign-in and Rich Presence without the SDK (net_discord_core.h): the shared token file, the application id,
 * the presence's text, the join secret, the fighters' names, the signed requests to the lobby's GSP pages and reading
 * their answers. Plain C, no Windows calls but the rename, so tools/dev/discordtest.c checks it on its own. */
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
    "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Sheik", "Zelda", "Dr. Mario",
    "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom", "Mr. Game & Watch",
    "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Pokemon Trainer", "Diddy Kong", "Lucas",
    "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf", "Villager", "Mega Man", "Wii Fit Trainer",
    "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt", "Ryu",
    "Ken", "Cloud", "Corrin", "Bayonetta", "Richter", "Inkling", "Ridley", "King K. Rool", "Simon", "Isabelle",
    "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth",
    "Min Min", "Steve", "Sephiroth", "Pyra & Mythra", "Kazuya", "Sora"};   /* (by the GSP records' fighter number - DataStore 82's u8 -
 * which is the game's save_no in ui_chara_db, not the fighter kind: 16 Ice Climbers, 29 Mr. Game & Watch (issue #389),
 * 37 Pokemon Trainer; the names the lobby's /gsp/me gives a number for) */

const char *discord_fighter_name(int kind) {
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

uint64_t discord_app_id_parse(const char *text) {
    if (!text) return 0;
    const char *p = text;
    while (*p) {
        const char *end = p + strcspn(p, "\r\n");
        const char *s = p;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s < end && *s != '#') {
            const char *eq = memchr(s, '=', (size_t)(end - s));
            const char *v = eq ? eq + 1 : s;
            if (!eq || ((size_t)(eq - s) >= 13 && !strncmp(s, "ApplicationId", 13))) {
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                uint64_t id = 0;
                int digits = 0;
                for (; v < end && *v >= '0' && *v <= '9' && digits < 20; v++, digits++) id = id * 10 + (uint64_t)(*v - '0');
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                if (digits >= 15 && v == end) return id;   /* (Discord's ids: 17-20 digits) */
            }
        }
        p = *end ? end + 1 : end;
    }
    return 0;
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
    char free_[32] = "";
    if (p->max_players > 0 && p->players < p->max_players && !p->in_match)
        snprintf(free_, sizeof free_, " - %d free", p->max_players - p->players);
    switch (p->where) {
    case DP_QUICKPLAY: snprintf(state, sn, "%s", p->in_match ? "In a match" : "Looking for a match"); break;
    case DP_ARENA:
        if (p->target[0]) snprintf(state, sn, "Arena %s%s%s", p->target, p->in_match ? " - in a match" : "", free_);
        else snprintf(state, sn, "%s", p->in_match ? "In a match" : "Looking for an arena");
        break;
    case DP_ROOM: snprintf(state, sn, "%s%s", p->in_match ? "In a match" : "Waiting in the room", free_); break;
    case DP_LOCAL: snprintf(state, sn, "%s", p->in_match ? "In a match" : ""); break;
    default: snprintf(state, sn, "%s", ""); break;
    }
}

static int secret_part(const char *s, size_t cap) {   /* printable ASCII without spaces or the separator, and fits */
    size_t n = strlen(s);
    if (n >= cap) return 0;
    for (const char *c = s; *c; c++) if ((unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e || *c == '|') return 0;
    return 1;
}

size_t discord_join_secret(const DiscordPresence *p, char *out, size_t cap) {
    if (cap) out[0] = 0;
    char kind = p->where == DP_ARENA ? 'a' : p->where == DP_ROOM ? 'r' : 0;
    DiscordJoin j;
    if (!kind || !p->target[0] || !p->relay[0] || !secret_part(p->relay, sizeof j.relay) || !secret_part(p->code, sizeof j.code)
        || !secret_part(p->target, sizeof j.target))
        return 0;
    if (p->max_players > 0 && p->players >= p->max_players) return 0;
    int n = snprintf(out, cap, "os2|%c|%s|%s|%s", kind, p->relay, p->code, p->target);
    if (n <= 0 || (size_t)n >= cap || n > 128) { if (cap) out[0] = 0; return 0; }   /* (Discord: 128 characters at most) */
    return (size_t)n;
}

static const char *take_part(const char *s, char *out, size_t cap, int last) {   /* the part up to '|' (or the end, last) */
    const char *bar = last ? s + strlen(s) : strchr(s, '|');
    if (!bar) return NULL;
    size_t n = (size_t)(bar - s);
    if (n >= cap) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    if (!secret_part(out, cap)) return NULL;
    return last ? bar : bar + 1;
}

int discord_join_parse(const char *secret, DiscordJoin *j) {
    memset(j, 0, sizeof *j);
    if (!secret || strncmp(secret, "os2|", 4)) return 0;
    const char *k = secret + 4;
    if ((k[0] != 'a' && k[0] != 'r') || k[1] != '|') return 0;
    const char *p = k + 2;
    if (!(p = take_part(p, j->relay, sizeof j->relay, 0)) || !(p = take_part(p, j->code, sizeof j->code, 0))
        || !take_part(p, j->target, sizeof j->target, 1) || !j->relay[0] || !j->target[0]) {
        memset(j, 0, sizeof *j);
        return 0;
    }
    j->where = k[0] == 'a' ? DP_ARENA : DP_ROOM;
    return 1;
}

/* ---- SHA-256 and HMAC (FIPS 180-4, RFC 2104) ------------------------------------------------ */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; } Sha;
static void sha_block(Sha *s, const uint8_t *b) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16 | (uint32_t)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K256[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof *a);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(Sha *s) {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, H0, sizeof H0);
    s->len = 0;
    s->used = 0;
}
static void sha_add(Sha *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += k; p += k; n -= k;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}
static void sha_end(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha_add(s, &pad, 1);
    while (s->used != 56) sha_add(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_add(s, len, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i]; }
}
void discord_sha256(const void *data, size_t n, uint8_t out[32]) {
    Sha s;
    sha_init(&s);
    sha_add(&s, data, n);
    sha_end(&s, out);
}
void discord_hmac_sha256(const void *key, size_t kn, const void *msg, size_t mn, uint8_t out[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    if (kn > 64) discord_sha256(key, kn, k); else memcpy(k, key, kn);
    Sha s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&s); sha_add(&s, pad, 64); sha_add(&s, msg, mn); sha_end(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&s); sha_add(&s, pad, 64); sha_add(&s, inner, 32); sha_end(&s, out);
}
static void hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = d[b[i] >> 4]; out[i * 2 + 1] = d[b[i] & 15]; }
    out[n * 2] = 0;
}
static void hash16(const char *prefix, const char *v, char out[17]) {
    Sha s;
    uint8_t h[32];
    sha_init(&s);
    sha_add(&s, prefix, strlen(prefix));
    sha_add(&s, v, strlen(v));
    sha_end(&s, h);
    hex(h, 8, out);
}
void discord_player_id(const char *nsa_hex, char out[17]) { hash16("opensmash-player\n", nsa_hex, out); }
void discord_account_hash(const char *user_id, char out[17]) { hash16("opensmash-discord\n", user_id, out); }
void discord_sign(const char *nsa_hex, const char *what, const char *player, int64_t t, const char *payload, char out[65]) {
    char msg[512];
    int n = snprintf(msg, sizeof msg, "opensmash1\n%s\n%s\n%lld\n%s", what, player, (long long)t, payload ? payload : "");
    if (n < 0 || (size_t)n >= sizeof msg) { out[0] = 0; return; }
    uint8_t mac[32];
    discord_hmac_sha256(nsa_hex, strlen(nsa_hex), msg, (size_t)n, mac);
    hex(mac, 32, out);
}

/* ---- reading the pages' JSON: a small reader that walks values and skips what it does not want ---------------- */
static const char *ws(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; return p; }
/* past a string (p at its opening quote), its text into out (escapes but \" and \\ kept as written); NULL: cut short */
static const char *str(const char *p, char *out, size_t cap) {
    size_t o = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\') { if (!p[1]) return NULL; p++; }
        if (out && o + 1 < cap) out[o++] = *p;
    }
    if (out && cap) out[o] = 0;
    return *p ? p + 1 : NULL;
}
static const char *skip(const char *p, int depth) {   /* past one value; NULL: cut short or too deep */
    p = ws(p);
    if (!p || !*p || depth > 32) return NULL;
    if (*p == '"') return str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1);
        if (p && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                if (!p || *p != '"' || !(p = str(p, NULL, 0)) || *(p = ws(p)) != ':') return NULL;
                p++;
            }
            if (!(p = skip(p, depth + 1))) return NULL;
            p = ws(p);
            if (*p == ',') { p = ws(p + 1); continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) { while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9')) p++; return p; }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    return NULL;
}
/* Each member of an object (p at '{'): fn(key, value start, ctx); returns past the object, NULL when malformed. */
typedef void (*MemberFn)(const char *key, const char *value, void *ctx);
static const char *members(const char *p, MemberFn fn, void *ctx) {
    p = ws(p);
    if (!p || *p != '{') return NULL;
    p = ws(p + 1);
    if (*p == '}') return p + 1;
    for (;;) {
        char key[32];
        if (*p != '"' || !(p = str(p, key, sizeof key)) || *(p = ws(p)) != ':') return NULL;
        const char *v = ws(p + 1);
        fn(key, v, ctx);
        if (!(p = skip(v, 1))) return NULL;
        p = ws(p);
        if (*p == ',') { p = ws(p + 1); continue; }
        return *p == '}' ? p + 1 : NULL;
    }
}
static long number(const char *v) { return (*v == '-' || (*v >= '0' && *v <= '9')) ? strtol(v, NULL, 10) : 0; }

static void where_member(const char *key, const char *v, void *ctx) {
    DiscordMe *m = ctx;
    if (!strcmp(key, "mode") && *v == '"') { char mode[16]; str(v, mode, sizeof mode); m->where = !strcmp(mode, "arena") ? DP_ARENA : DP_QUICKPLAY; }
    else if (!strcmp(key, "arena") && *v == '"') str(v, m->arena, sizeof m->arena);
    else if (!strcmp(key, "players")) m->players = (int)number(v);
    else if (!strcmp(key, "max")) m->max_players = (int)number(v);
    else if (!strcmp(key, "open")) m->open = !strncmp(v, "true", 4);
}
static void me_member(const char *key, const char *v, void *ctx) {
    DiscordMe *m = ctx;
    if (!strcmp(key, "rank")) m->rank = (int)number(v);
    else if (!strcmp(key, "fighter")) m->fighter = (int)number(v);
    else if (!strcmp(key, "gsp")) m->gsp = (uint32_t)number(v);
    else if (!strcmp(key, "where") && *v == '{') { m->where = DP_QUICKPLAY; members(v, where_member, m); }
}
int discord_me_parse(const char *json, DiscordMe *m) {
    memset(m, 0, sizeof *m);
    m->fighter = -1;
    if (!json || !members(json, me_member, m)) { memset(m, 0, sizeof *m); m->fighter = -1; return 0; }
    for (char *c = m->arena; *c; c++) if (*c == '|' || (unsigned char)*c < 0x21 || (unsigned char)*c > 0x7e) { m->arena[0] = 0; break; }
    return 1;
}

typedef struct { const char *want; int rank; long r; char id[24]; } Find;
static void player_member(const char *key, const char *v, void *ctx) {
    Find *f = ctx;
    if (!strcmp(key, "id") && *v == '"') str(v, f->id, sizeof f->id);
    else if (!strcmp(key, "rank")) f->r = number(v);
}
static void board_member(const char *key, const char *v, void *ctx) {
    Find *f = ctx;
    if (strcmp(key, "players") || *v != '[' || f->rank) return;
    const char *p = ws(v + 1);
    while (p && *p == '{') {
        f->r = 0; f->id[0] = 0;
        if (!(p = members(p, player_member, f))) return;
        if (f->r > 0 && f->r <= 1000000 && !strcmp(f->id, f->want)) { f->rank = (int)f->r; return; }
        p = ws(p);
        if (*p != ',') return;
        p = ws(p + 1);
    }
}
int discord_rank_in_leaderboard(const char *json, const char *player_id) {
    if (!json || !player_id || !*player_id) return 0;
    Find f = {player_id, 0, 0, ""};
    members(json, board_member, &f);   /* (a file cut short still gives the rank found before the cut) */
    return f.rank;
}
