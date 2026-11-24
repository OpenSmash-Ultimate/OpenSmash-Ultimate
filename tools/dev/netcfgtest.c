/* Offline checks of where the game goes online (runtime/net_relay.c's configure): the lobby from
 * SSBU_NET_RELAY, or from online_server.txt when started with --online as Play SSBU.bat does,
 * and net_relay_address(), which the game's Online mode (hle_online.c) logs in through. Nothing
 * is sent: only the settings are read. Each case runs in a child process of its own (the settings
 * are read once a process), in a folder of its own.
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/dev/netcfgtest.c', '/Febuild/netcfgtest.exe'], check=True)"
 *   build\netcfgtest.exe
 *
 * (tests/test_settings.py builds and runs it.) Exit code 0 when every check passes. */
#include "../../runtime/net_relay.c"
#include "../../runtime/net_region.c"
#include "../../runtime/hle_settings.c"
#include "../../runtime/hle_config.c"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* (what net_relay.c reaches outside itself; never called here) */
const char *rt_data_dir = ".";
int rt_code_patches_on(void) { const char *e = getenv("SSBU_CODE_PATCHES"); return e && *e == '1'; }   /* (hle_mod_switch.c: SSBU_CODE_PATCHES here) */
const RtModule rt_module_main;
void vsock_deliver(u32 src_ip, u16 src_port, u32 dst_ip, u16 dst_port, const void *data, u32 len) {}
int ldn_in_network(u32 ip) { return 0; }
void ldn_listing(u32 seq, u32 owner, const u8 *info) {}
void ldn_list_end(u32 seq) {}
int pad_ini_convert(const char *from, const char *to) { return 0; }   /* (hle_config.c's, for an ssbu-recomp install: none here) */
const char *display_default_ini(void) { return ""; }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct Case {
    const char *name, *server_file, *env, *args;   /* server_file NULL: none; env: NAME=value or NULL */
    const char *address, *room;                     /* what net_relay_address() and the room are; address NULL: none */
    /* the server region (net_region.c): the lobbies' answer times instead of asking them (NULL: PINGS, below),
     * config/server_region.txt, config/server_region_auto.txt (Auto's last pick) and that file's first word afterwards,
     * a friend's Join from Discord ("<r|a> <lobby> <code> <target>", config/discord_join.txt) */
    const char *pings, *region, *kept, *kept_after, *join;
    /* direct paths (net_relay.c direct_ok): never with a lobby server unless SSBU_NET_DIRECT=1; in a private room
     * (hosting, This network) on unless the Direct connections setting, online_direct.txt, says off */
    int direct;
    const char *direct_file;
} Case;
#define PINGS "eu=20,na-east=90"   /* (every case: nothing is sent to a lobby by this test) */
#define NA_LOBBY "na.lobby.opensmashultimate.com:39421"

static const Case CASES[] = {
    {"auto in the file: the official lobby (Auto: Europe answers sooner)", "# the lobby\nauto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"na (the older North American lobby's word): the official lobby", "na\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"eu", "eu\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"the older North American lobby's address alone: the official lobby", "smash.recomps.wtf:39421\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"the older North American lobby's address with a code: the official lobby, the code kept", "smash.recomps.wtf:39421 OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode"},
    {"an older public lobby's address with a code: the official lobby, the code kept", "opensmash-lobby.duckdns.org:39421 OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode"},
    {"the official lobby's name with a code", NET_OFFICIAL_LOBBY " OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode"},
    {"SSBU_OFFICIAL_LOBBY: another official lobby, the default name still means it", NET_OFFICIAL_LOBBY " OurCode\n", "SSBU_OFFICIAL_LOBBY=lobby.example:39421", "--online", "lobby.example:39421", "OurCode"},
    {"a server of the player's own is kept", "smash.example:39421 friends\n", NULL, "--online", "smash.example:39421", "friends"},
    {"offline in the file", "off\n", NULL, "--online", NULL, "smash"},
    {"no file", NULL, NULL, "--online", NULL, "smash"},
    {"no --online: the file is not read", "auto\n", NULL, "", NULL, "smash"},
    {"SSBU_NET_RELAY=na (the macOS / Linux launchers pass the file's word, #259)", "na\n", "SSBU_NET_RELAY=na", "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"SSBU_NET_RELAY=auto", "auto\n", "SSBU_NET_RELAY=auto", "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"SSBU_NET_RELAY wins", "auto\n", "SSBU_NET_RELAY=127.0.0.1:39420", "--online", "127.0.0.1:39420", "smash"},
    {"SSBU_NET_RELAY=off wins", "auto\n", "SSBU_NET_RELAY=off", "--online", NULL, "smash"},
    {"hosting a private room: no lobby", "auto\n", "SSBU_NET_HOST=1", "--online", NULL, "smash", .direct = 1},
    {"lan in the file: a private room found on this network, no lobby", "lan\n", NULL, "--online", NULL, "smash", .direct = 1},
    {"hosting a private room, Direct connections off: through the host's game alone", "auto\n", "SSBU_NET_HOST=1", "--online", NULL, "smash", .direct = 0, .direct_file = "off\n"},
    {"lan, Direct connections off", "lan\n", NULL, "--online", NULL, "smash", .direct = 0, .direct_file = "off\n"},
    {"lan, Direct connections on", "lan\n", NULL, "--online", NULL, "smash", .direct = 1, .direct_file = "on\n"},
    {"the official lobby, Direct connections on in the file: relay only all the same", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash", .direct = 0, .direct_file = "on\n"},
    {"a server of the player's own, Direct connections on in the file: not asked for by the file", "smash.example:39421 friends\n", NULL, "--online", "smash.example:39421", "friends", .direct = 0, .direct_file = "on\n"},
    {"lan and a room code in the file", "lan ourroom\n", NULL, "--online", NULL, "ourroom", .direct = 1},
    {"SSBU_NET_RELAY=lan", "auto\n", "SSBU_NET_RELAY=lan", "--online", NULL, "smash", .direct = 1},
    /* ---- the server region */
    {"Auto: North America answers sooner", "auto\n", NULL, "--online", NA_LOBBY, "smash", "eu=90,na-east=20", NULL, NULL, "na-east"},
    {"Auto: only North America answers", "auto\n", NULL, "--online", NA_LOBBY, "smash", "eu=-1,na-east=140", NULL, NULL, "na-east"},
    {"Auto: no lobby answers: the first region, and no pick kept", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash", "eu=-1,na-east=-1", NULL, NULL, ""},
    {"Auto: the same time: the first region", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash", "eu=40,na-east=40", NULL, NULL, "eu"},
    {"Auto with the last pick kept: played there at once, measured again for the next start", "auto\n", NULL, "--online", NA_LOBBY, "smash", NULL, "auto\n", "na-east 31 1700000000\n", "eu"},
    {"Auto with the last pick kept and the other region a little sooner: it stays (no hopping between two close ones)", "auto\n", NULL, "--online", NA_LOBBY, "smash", "eu=52,na-east=60", NULL, "na-east 31 1700000000\n", "na-east"},
    {"Auto with a kept pick that is no region: measured", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash", NULL, NULL, "mars 5 1\n", "eu"},
    {"the region chosen: NA East, though Europe answers sooner", "auto\n", NULL, "--online", NA_LOBBY, "smash", NULL, "na-east\n", NULL, ""},
    {"the region chosen: EU, though North America answers sooner", "auto OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode", "eu=90,na-east=20", "# the region\nEU\n", NULL, ""},
    {"a region that is none: Auto", "auto\n", NULL, "--online", NA_LOBBY, "smash", "eu=90,na-east=20", "moon\n", NULL, "na-east"},
    {"a region's lobby in online_server.txt means the official lobby: the region setting says which", NA_LOBBY " OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode", NULL, "eu\n"},
    {"SSBU_NET_RELAY=auto follows the region", "off\n", "SSBU_NET_RELAY=auto", "--online", NA_LOBBY, "smash", NULL, "na-east\n"},
    {"an override wins over the region chosen", "auto\n", "SSBU_OFFICIAL_LOBBY=lobby.example:39421", "--online", "lobby.example:39421", "smash", "eu=90,na-east=20", "na-east\n", NULL, ""},
    {"an override that is a region's lobby: that server, no Auto", "auto\n", "SSBU_OFFICIAL_LOBBY=" NA_LOBBY, "--online", NA_LOBBY, "smash", NULL, "auto\n", NULL, ""},
    {"a server of the player's own: no lobby is asked, nothing kept", "smash.example:39421\n", NULL, "--online", "smash.example:39421", "smash", NULL, NULL, NULL, ""},
    {"a friend's Join from Discord on the other region: this start plays there", "auto\n", NULL, "--online", NA_LOBBY, "smash", NULL, "eu\n", NULL, "", "r " NA_LOBBY " - 10.13.0.7"},
    {"a friend's Join on the other region, a lobby code", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "abc", NULL, "na-east\n", NULL, "", "a " NET_OFFICIAL_LOBBY " abc K7Q2M"},
    {"a friend's Join with an override: the override's server", "auto\n", "SSBU_OFFICIAL_LOBBY=lobby.example:39421", "--online", "lobby.example:39421", "smash", NULL, NULL, NULL, "", "r " NA_LOBBY " - 10.13.0.7"},
    {"a Join for a lobby that is not official is ignored", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash", NULL, NULL, NULL, "eu", "r evil.example:39421 - 10.13.0.7"},
    /* ---- Epic Online Services, gone in 0.5.0: "eos" (the file, SSBU_NET_RELAY, an older game's Join) is the official lobby */
    {"eos in the file: the official lobby", "eos\n", NULL, "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"eos and a lobby code: the official lobby, the code kept", "eos OurCode\n", NULL, "--online", NET_OFFICIAL_LOBBY, "OurCode"},
    {"SSBU_NET_RELAY=eos: the official lobby", "auto\n", "SSBU_NET_RELAY=eos", "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"SSBU_EOS_FIRST=1 means nothing now: the official lobby", "auto\n", "SSBU_EOS_FIRST=1", "--online", NET_OFFICIAL_LOBBY, "smash"},
    {"SSBU_EOS=1 and a server of the player's own: that server", "smash.example:39421\n", "SSBU_EOS=1", "--online", "smash.example:39421", "smash"},
    {"hosting a private room, eos in the file: no lobby", "eos\n", "SSBU_NET_HOST=1", "--online", NULL, "smash", .direct = 1},
    {"an older game's Join over EOS: the official lobby, its lobby code", "auto\n", NULL, "--online", NET_OFFICIAL_LOBBY, "abc", NULL, NULL, NULL, NULL, "r eos abc 10.13.0.7"},
};

/* The child: reads the settings as the game does and writes what it found to result.txt. */
static int child(int argc, char **argv) {
    settings_args(argc, argv);
    const char *a = net_relay_address();
    FILE *f = fopen("result.txt", "wb");
    if (!f) return 1;
    fprintf(f, "%s\n%s\n%d\n", a ? a : "(none)", room, direct_ok);
    fclose(f);
    for (int i = 0; region_remeasuring && i < 1000; i++) Sleep(5);   /* (net_region.c: Auto's pick for the next start, being kept) */
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--child")) return child(argc, argv);
    static const char *const VARS[] = {"SSBU_NET_RELAY", "SSBU_NET_HOST", "SSBU_NET_ROOM", "SSBU_NET_NAME", "SSBU_NET_DIRECT", "SSBU_OFFICIAL_LOBBY", "SSBU_EOS", "SSBU_EOS_FIRST"};   /* (SSBU_EOS*: gone in 0.5.0, cleared all the same) */
    for (int v = 0; v < (int)(sizeof VARS / sizeof *VARS); v++) SetEnvironmentVariableA(VARS[v], NULL);
    char exe[MAX_PATH], base[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetTempPathA(sizeof base, base);
    for (int k = 0; k < (int)(sizeof CASES / sizeof *CASES); k++) {
        const Case *t = &CASES[k];
        char dir[MAX_PATH + 32], file[MAX_PATH + 64], cmd[2 * MAX_PATH], env[128] = "";
        snprintf(dir, sizeof dir, "%snetcfg%lu_%d", base, GetCurrentProcessId(), k);
        CreateDirectoryA(dir, NULL);
        if (t->server_file) {
            snprintf(file, sizeof file, "%s\\online_server.txt", dir);
            FILE *f = fopen(file, "wb");
            fputs(t->server_file, f);
            fclose(f);
        }
        static const char *const REGION_FILES[] = {"server_region.txt", "server_region_auto.txt", "discord_join.txt"};
        const char *region_text[] = {t->region, t->kept, t->join};
        snprintf(file, sizeof file, "%s\\config", dir);
        CreateDirectoryA(file, NULL);
        for (int r = 0; r < 3; r++)
            if (region_text[r]) {
                snprintf(file, sizeof file, "%s\\config\\%s", dir, REGION_FILES[r]);
                FILE *f = fopen(file, "wb");
                fputs(region_text[r], f);
                if (r == 2) fprintf(f, " %lld\n", (long long)time(NULL));
                fclose(f);
            }
        if (t->direct_file) {
            snprintf(file, sizeof file, "%s\\config\\online_direct.txt", dir);
            FILE *f = fopen(file, "wb");
            fputs(t->direct_file, f);
            fclose(f);
        }
        SetEnvironmentVariableA("SSBU_REGION_TEST_PINGS", t->pings ? t->pings : PINGS);
        char *eq = NULL;
        if (t->env) { snprintf(env, sizeof env, "%s", t->env); eq = strchr(env, '='); *eq = 0; SetEnvironmentVariableA(env, eq + 1); }
        snprintf(cmd, sizeof cmd, "\"%s\" --child %s", exe, t->args);
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (t->env) SetEnvironmentVariableA(env, NULL);
        char got_address[300] = "", got_room[64] = "", got_direct[16] = "";
        snprintf(file, sizeof file, "%s\\result.txt", dir);
        FILE *f = fopen(file, "rb");
        if (f) {
            if (fgets(got_address, sizeof got_address, f)) got_address[strcspn(got_address, "\r\n")] = 0;
            if (fgets(got_room, sizeof got_room, f)) got_room[strcspn(got_room, "\r\n")] = 0;
            if (fgets(got_direct, sizeof got_direct, f)) got_direct[strcspn(got_direct, "\r\n")] = 0;
            fclose(f);
        }
        CHECK(code == 0 && f, "%s: the child did not run (exit %lu)", t->name, code);
        const char *want = t->address ? t->address : "(none)";
        CHECK(!strcmp(got_address, want), "%s: the lobby address is '%s', wanted '%s'", t->name, got_address, want);
        CHECK(!strcmp(got_room, t->room), "%s: the lobby code is '%s', wanted '%s'", t->name, got_room, t->room);
        CHECK(atoi(got_direct) == t->direct, "%s: direct paths %s, wanted %d", t->name, got_direct, t->direct);
        if (t->kept_after) {   /* (Auto's pick kept for the next start: its region; "": no file) */
            char kept[64] = "", word[32] = "";
            snprintf(file, sizeof file, "%s\\config\\server_region_auto.txt", dir);
            FILE *k = fopen(file, "rb");
            if (k) { if (fgets(kept, sizeof kept, k)) sscanf(kept, "%31s", word); fclose(k); }
            CHECK(!strcmp(word, t->kept_after), "%s: the pick kept is '%s', wanted '%s'", t->name, word, t->kept_after);
        }
        for (int r = 0; r < 3; r++) {
            snprintf(file, sizeof file, "%s\\config\\%s", dir, REGION_FILES[r]);
            DeleteFileA(file);
        }
        snprintf(file, sizeof file, "%s\\result.txt", dir);
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\online_server.txt", dir);
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\config\\online_direct.txt", dir);
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\config\\online_name.txt", dir);   /* (picked by the game when online) */
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\config\\version.ini", dir);   /* (the settings folder, made at the first start) */
        DeleteFileA(file);
        snprintf(file, sizeof file, "%s\\config", dir);
        RemoveDirectoryA(file);
        RemoveDirectoryA(dir);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
