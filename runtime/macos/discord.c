/* Discord Rich Presence (macOS): who is fighting whom, on the player's Discord profile.
 *
 * Off unless discord.txt in the port's folder names a Discord application ID (its first line that
 * is not a comment; "off" turns it off; SSBU_DISCORD overrides the file). The Discord app listens
 * on a Unix socket, $TMPDIR/discord-ipc-N: a frame is a little-endian opcode and length, then
 * JSON. Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a command (SET_ACTIVITY). A thread
 * of its own connects (again, whenever Discord was not running), and sends the activity when it
 * changes. Nothing here waits on Discord on a game thread.
 *
 * The match. The fighters' code (the lua2cpp NROs, and main's own Lua bindings) asks the game's
 * FighterManager through exported functions (app::lua_bind::FighterManager__*_impl) that take the
 * manager as their first argument. discord_init binds those names to wrappers before the modules
 * are linked: a wrapper notes the manager, at most once a second reads the match through the
 * same exported functions - entries, each one's FighterInformation (CPU or not, stocks) and its
 * fighter's battle object (app::sv_battle_object::kind) - on that guest thread, its registers
 * kept, then makes the call it wrapped. Calls into the game made here go to the functions
 * themselves, not the wrappers. */
#include "host.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

u64 rt_wrap_symbol(const char *name, GuestFn *fn);   /* (host.c) */

/* ---- the game's functions ---------------------------------------------------------- */
enum { F_ENTRY_COUNT, F_ENTRY_ID, F_INFO, F_ENTRY, F_RESULT, F_WRAPPED,   /* (the manager's: wrapped) */
       F_IS_CPU = F_WRAPPED, F_STOCKS, F_CURRENT, F_IS_NULL, F_KIND, F_COUNT };
static const char *const SYMS[F_COUNT] = {
    "_ZN3app8lua_bind32FighterManager__entry_count_implEPNS_14FighterManagerE",
    "_ZN3app8lua_bind33FighterManager__get_entry_id_implEPNS_14FighterManagerEi",
    "_ZN3app8lua_bind44FighterManager__get_fighter_information_implEPNS_14FighterManagerENS_14FighterEntryIDE",
    "_ZN3app8lua_bind38FighterManager__get_fighter_entry_implEPNS_14FighterManagerENS_14FighterEntryIDE",
    "_ZN3app8lua_bind35FighterManager__is_result_mode_implEPNS_14FighterManagerE",
    "_ZN3app8lua_bind41FighterInformation__is_operation_cpu_implEPNS_18FighterInformationE",
    "_ZN3app8lua_bind36FighterInformation__stock_count_implEPNS_18FighterInformationE",
    "_ZN3app8lua_bind37FighterEntry__current_fighter_id_implEPNS_12FighterEntryE",
    "_ZN3app16sv_battle_object7is_nullEj",
    "_ZN3app16sv_battle_object4kindEj",
};
static u64 real[F_COUNT];

/* FIGHTER_KIND_*: the game's numbers for its fighters (an unknown one shows as its number) */
static const char *const KINDS[] = {
    "Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
    "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Ice Climbers", "Sheik", "Zelda",
    "Dr. Mario", "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom",
    "Mr. Game & Watch", "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike", "Squirtle", "Ivysaur",
    "Charizard", "Diddy Kong", "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.", "Toon Link", "Wolf",
    "Villager", "Mega Man", "Wii Fit Trainer", "Rosalina & Luma", "Little Mac", "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk",
    "Bowser Jr.", "Duck Hunt", "Ryu", "Ken", "Cloud", "Corrin", "Bayonetta", "Inkling", "Ridley", "Simon",
    "Richter", "King K. Rool", "Isabelle", "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner", NULL, NULL, NULL,
    NULL, "Piranha Plant", "Joker", "Hero", "Banjo & Kazooie", "Terry", "Byleth", "Min Min", "Steve", "Sephiroth",
    "Pyra", "Mythra", "Kazuya", "Sora",
};

/* ---- the match, as last read ------------------------------------------------------- */
typedef struct Player { int kind, cpu, stocks; } Player;
typedef struct Match { int n, result; Player p[8]; u64 at; } Match;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Match match;
static volatile u64 manager, last_read;
static _Thread_local int reading;

static u64 call(Cpu *c, int f, u64 a, u64 b) {
    c->x[0] = a;
    c->x[1] = b;
    rt_call(c, real[f]);
    return c->x[0];
}

static void read_match(Cpu *c) {
    Match m = {0};
    u64 mgr = manager;
    int n = (int)(u32)call(c, F_ENTRY_COUNT, mgr, 0);
    if (n < 0 || n > 8) n = 0;
    for (int i = 0; i < n; i++) {
        u32 id = (u32)call(c, F_ENTRY_ID, mgr, (u64)i);
        u64 info = call(c, F_INFO, mgr, id), entry = call(c, F_ENTRY, mgr, id);
        if (!info || !entry) continue;
        u32 object = (u32)call(c, F_CURRENT, entry, 0);
        if (call(c, F_IS_NULL, object, 0) & 1) continue;
        m.p[m.n].kind = (int)(u32)call(c, F_KIND, object, 0);
        m.p[m.n].cpu = (int)(call(c, F_IS_CPU, info, 0) & 1);
        m.p[m.n].stocks = (int)(u32)call(c, F_STOCKS, info, 0);
        m.n++;
    }
    m.result = n ? (int)(call(c, F_RESULT, mgr, 0) & 1) : 0;
    m.at = rt_now_ns();
    pthread_mutex_lock(&lock);
    match = m;
    pthread_mutex_unlock(&lock);
}

/* A wrapper: the manager noted, the match read at most once a second, then the call itself */
static void seen(Cpu *c, int f) {
    if (!reading) {
        manager = c->x[0];
        u64 now = rt_now_ns(), last = last_read;
        if (now - last > 1000000000ull && __atomic_compare_exchange_n(&last_read, &last, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            Cpu saved = *c;
            reading = 1;
            read_match(c);
            reading = 0;
            *c = saved;
        }
    }
    rt_call(c, real[f]);
}
static void wrap_entry_count(Cpu *c) { seen(c, F_ENTRY_COUNT); }
static void wrap_entry_id(Cpu *c) { seen(c, F_ENTRY_ID); }
static void wrap_info(Cpu *c) { seen(c, F_INFO); }
static void wrap_entry(Cpu *c) { seen(c, F_ENTRY); }
static void wrap_result(Cpu *c) { seen(c, F_RESULT); }

/* ---- Discord's socket ---------------------------------------------------------------- */
static char client_id[32];
static int sock = -1;

static int put(int op, const char *json) {
    u32 h[2] = {(u32)op, (u32)strlen(json)};
    if (send(sock, h, sizeof h, 0) != (ssize_t)sizeof h || send(sock, json, h[1], 0) != (ssize_t)h[1]) return 0;
    return 1;
}

/* One frame back (its body thrown away): its opcode, or -1 */
static int get(void) {
    u32 h[2];
    if (recv(sock, h, sizeof h, MSG_WAITALL) != (ssize_t)sizeof h) return -1;
    char buf[1024];
    for (u32 left = h[1]; left;) {
        ssize_t got = recv(sock, buf, left < sizeof buf ? left : sizeof buf, 0);
        if (got <= 0) return -1;
        left -= (u32)got;
    }
    return (int)h[0];
}

static void disconnect(void) {
    if (sock >= 0) close(sock);
    sock = -1;
}

static int connect_discord(void) {
    const char *dirs[] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp"};
    for (unsigned d = 0; d < sizeof dirs / sizeof *dirs; d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (int i = 0; i < 10; i++) {
            struct sockaddr_un a = {.sun_family = AF_UNIX};
            size_t len = strlen(dirs[d]);
            snprintf(a.sun_path, sizeof a.sun_path, "%.*s/discord-ipc-%d", (int)(len && dirs[d][len - 1] == '/' ? len - 1 : len), dirs[d], i);
            if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return 0;
            int on = 1;
            setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            struct timeval tv = {3, 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            if (connect(sock, (struct sockaddr *)&a, sizeof a) == 0) {
                char hello[96];
                snprintf(hello, sizeof hello, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
                if (put(0, hello) && get() == 1) return 1;
            }
            disconnect();
        }
    }
    return 0;
}

/* ---- the activity -------------------------------------------------------------------- */
static void name_of(int kind, char *out, size_t n) {
    if (kind >= 0 && kind < (int)(sizeof KINDS / sizeof *KINDS) && KINDS[kind]) snprintf(out, n, "%s", KINDS[kind]);
    else snprintf(out, n, "Fighter %d", kind);
}

/* "Joker (you) vs Mario (CPU)", "Mario vs Link vs Kirby", ... and the state line under it */
static void describe(const Match *m, char *details, size_t dn, char *state, size_t sn) {
    int humans = 0, you = -1;
    for (int i = 0; i < m->n; i++) if (!m->p[i].cpu) { humans++; if (you < 0) you = i; }
    details[0] = state[0] = 0;
    size_t at = 0;
    for (int i = 0; i < m->n && at < dn; i++) {
        char name[48];
        name_of(m->p[i].kind, name, sizeof name);
        const char *tag = humans == 1 && i == you ? " (you)" : humans >= 1 && m->p[i].cpu ? " (CPU)" : "";
        at += (size_t)snprintf(details + at, dn - at, "%s%s%s", i ? " vs " : "", name, tag);
    }
    if (humans == 0) { snprintf(state, sn, "Watching a demo"); return; }
    if (m->result) { snprintf(state, sn, "Results"); return; }
    if (humans == 1 && m->p[you].stocks > 0 && m->p[you].stocks < 100)
        snprintf(state, sn, "%d stock%s left", m->p[you].stocks, m->p[you].stocks == 1 ? "" : "s");
    else snprintf(state, sn, "In a match");
}

static void json_string(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 3 < n; s++) {
        if (*s == '"' || *s == '\\') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
}

static void *presence(void *unused) {
    (void)unused;
    long long session = (long long)time(NULL), since = session;
    int was_fighting = 0, nonce = 0;
    char last[1024] = "", lineup[256] = "";
    u64 sent_at = 0;
    for (;; sleep(2)) {
        if (sock < 0 && !connect_discord()) { last[0] = 0; sleep(13); continue; }
        Match m;
        pthread_mutex_lock(&lock);
        m = match;
        pthread_mutex_unlock(&lock);
        int fighting = m.n >= 2 && rt_now_ns() - m.at < 4000000000ull;
        char details[256], state[128];
        if (fighting) {
            describe(&m, details, sizeof details, state, sizeof state);
            if (!was_fighting) since = (long long)time(NULL);
            char kinds[128] = "";
            for (int i = 0, o = 0; i < m.n; i++) o += snprintf(kinds + o, sizeof kinds - (size_t)o, " %d%s", m.p[i].kind, m.p[i].cpu ? "c" : "");
            if (strcmp(kinds, lineup)) { fprintf(stderr, "[discord] match: %s (kinds%s)\n", details, kinds); snprintf(lineup, sizeof lineup, "%s", kinds); }
        } else {
            snprintf(details, sizeof details, "In the menus");
            snprintf(state, sizeof state, "%s", getenv("SSBU_NET_RELAY") && *getenv("SSBU_NET_RELAY") ? "Online" : "Offline");
            if (was_fighting) since = session;
            lineup[0] = 0;
        }
        was_fighting = fighting;
        char d[300], s[160], activity[1024];
        json_string(d, sizeof d, details);
        json_string(s, sizeof s, state);
        snprintf(activity, sizeof activity,
                 "{\"details\":\"%.127s\",\"state\":\"%.127s\",\"timestamps\":{\"start\":%lld},"
                 "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"WindowSmash on macOS\"}}", d, s, since);
        if (!strcmp(activity, last) && rt_now_ns() - sent_at < 60000000000ull) continue;
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":%s},\"nonce\":\"%d\"}",
                 (int)getpid(), activity, ++nonce);
        if (!put(1, cmd) || get() < 0) { disconnect(); continue; }
        snprintf(last, sizeof last, "%s", activity);
        sent_at = rt_now_ns();
    }
    return NULL;
}

/* (the loader, before the modules are linked) on, if discord.txt or SSBU_DISCORD names an app */
void discord_init(void) {
    const char *env = getenv("SSBU_DISCORD");
    char line[128] = "";
    if (env) snprintf(line, sizeof line, "%s", env);
    else {
        FILE *f = fopen("discord.txt", "r");
        while (f && fgets(line, sizeof line, f) && (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r')) line[0] = 0;
        if (f) fclose(f);
    }
    size_t n = 0;
    for (const char *p = line; *p >= '0' && *p <= '9' && n + 1 < sizeof client_id; p++) client_id[n++] = *p;
    client_id[n] = 0;
    if (n < 15) return;   /* (no file, "off", or not an application ID) */
    for (int f = 0; f < F_COUNT; f++)
        if (!rt_symbol(SYMS[f])) { fprintf(stderr, "[discord] off: this game version has no %s\n", SYMS[f]); return; }
    GuestFn *wraps[F_WRAPPED] = {wrap_entry_count, wrap_entry_id, wrap_info, wrap_entry, wrap_result};
    for (int f = 0; f < F_COUNT; f++) real[f] = f < F_WRAPPED ? rt_wrap_symbol(SYMS[f], wraps[f]) : rt_symbol(SYMS[f]);
    pthread_t t;
    if (pthread_create(&t, NULL, presence, NULL) == 0) pthread_detach(t);
    fprintf(stderr, "[discord] rich presence on (application %s)\n", client_id);
}
