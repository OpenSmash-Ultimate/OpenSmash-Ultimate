/* The router's mapping of this game's own UDP port in LOBBY mode, for direct paths through routers that change ports
 * (net_relay.c, "Routers that change ports"): PCP, then NAT-PMP (RFC 6887, 6886: one UDP question to the gateway's
 * port 5351), then UPnP-IGD (net_upnp.c), asked once the game is in a lobby. The outside port goes to the lobby in the
 * PINGs ('N' 'P'), which tells the room's other players. PCP's and NAT-PMP's mapping is renewed while the game runs; every
 * mapping is taken back at exit. Off with online_upnp = off (SSBU_ONLINE_UPNP=0); never for a lobby on this PC or this
 * network. Same approach as WindowSmash's runtime/net_relay.c map_thread (our own code).
 *
 * #included by net_relay.c after net_upnp.c: everything static; g_sock's port (g_lan_port), g_lobby, g_upnp, say,
 * outbound_ip, test_loopback and the g_map_* state come from there. */
#ifndef NET_PMP_C
#define NET_PMP_C
#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")      /* (GetBestRoute: the gateway) */
#endif

static struct sockaddr_in g_map_gw;
static int g_map_pcp;
static char g_map_how[8];
static uint8_t g_map_nonce[12];

static uint32_t gateway_ip(void) {                       /* host order; 0 unknown */
    uint32_t gw = 0;
#ifdef _WIN32
    MIB_IPFORWARDROW row;
    if (GetBestRoute(htonl(0x08080808u), 0, &row) == NO_ERROR) gw = ntohl(row.dwForwardNextHop);
#elif defined(__linux__)
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], ifname[64];
    unsigned dest, g, flags;
    if (f) {
        while (!gw && fgets(line, sizeof line, f))
            if (sscanf(line, "%63s %x %x %x", ifname, &dest, &g, &flags) == 4 && dest == 0 && (flags & 2)) gw = ntohl(g);
        fclose(f);
    }
#endif
    if (!gw) { uint32_t me = outbound_ip(); if (me) gw = (me & 0xFFFFFF00u) | 1; }   /* (most home routers: .1 of the PC's network) */
    return gw;
}

/* One PCP MAP or NAT-PMP request for UDP port lp (life 0: take it back): the answer's outside port, or 0. */
static uint16_t pmp_ask(SOCKET s, uint16_t lp, uint16_t want, uint32_t life, int pcp) {
    uint8_t q[60], a[1100];
    int n;
    memset(q, 0, sizeof q);
    if (pcp) {                                           /* the header, then MAP's: nonce, protocol 17, ports, outside address wanted */
        q[0] = 2; q[1] = 1;
        s32(q + 4, life);
        q[18] = q[19] = 0xFF;
        s32(q + 20, outbound_ip());
        memcpy(q + 24, g_map_nonce, 12);
        q[36] = 17;
        s16(q + 40, lp);
        s16(q + 42, want);
        q[54] = q[55] = 0xFF;
        n = 60;
    } else {                                             /* version 0, opcode 1 UDP, internal port, outside port wanted, lifetime */
        q[1] = 1;
        s16(q + 4, lp);
        s16(q + 6, want);
        s32(q + 8, life);
        n = 12;
    }
    for (int tries = 0, wait = 250; tries < 3; tries++, wait *= 2) {
        sendto(s, (const char *)q, n, 0, (const struct sockaddr *)&g_map_gw, sizeof g_map_gw);
        if (!life) return 0;
        uint64_t end = ms_now() + (uint64_t)wait;
        while (ms_now() < end) {
            struct sockaddr_in from;
            int flen = sizeof from;
            int got = recvfrom(s, (char *)a, sizeof a, 0, (struct sockaddr *)&from, &flen);
            if (got <= 0 || from.sin_addr.s_addr != g_map_gw.sin_addr.s_addr) continue;
            if (pcp && got >= 60 && a[0] == 2 && a[1] == 0x81 && !memcmp(a + 24, g_map_nonce, 12)) return a[3] == 0 ? g16(a + 42) : 0;
            if (pcp && got >= 2 && a[0] == 0) return 0;  /* (a NAT-PMP gateway: PCP unsupported) */
            if (!pcp && got >= 16 && a[0] == 0 && a[1] == 129 && g16(a + 2) == 0 && g16(a + 8) == lp) return g16(a + 10);
            if (!pcp && got >= 4 && a[0] == 0 && a[1] == 129) return 0;
        }
    }
    return 0;
}

static uint16_t g_map_local;
static void map_close(void) {
    if (g_map_state != 2) return;
    if (!strcmp(g_map_how, "UPnP")) { upnp_unmap_udp(g_map_local); return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    pmp_ask(s, g_map_local, 0, 0, g_map_pcp);
    closesocket(s);
}

static DWORD WINAPI map_thread(LPVOID unused) {
    (void)unused;
    uint16_t lp = g_map_local, ext = 0;
    memset(&g_map_gw, 0, sizeof g_map_gw);
    g_map_gw.sin_family = AF_INET;
    g_map_gw.sin_port = htons(5351);
    g_map_gw.sin_addr.s_addr = htonl(gateway_ip());
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        DWORD timeout = 100;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        uint64_t r = rnd64(), r2 = rnd64();
        memcpy(g_map_nonce, &r, 8); memcpy(g_map_nonce + 8, &r2, 4);
        if (g_map_gw.sin_addr.s_addr && (ext = pmp_ask(s, lp, lp, 7200, 1))) { g_map_pcp = 1; snprintf(g_map_how, sizeof g_map_how, "PCP"); }
        else if (g_map_gw.sin_addr.s_addr && (ext = pmp_ask(s, lp, lp, 7200, 0))) snprintf(g_map_how, sizeof g_map_how, "NAT-PMP");
    }
    if (!ext) {
        char pub[32] = "", why[128] = "";
        uint32_t lan = outbound_ip();
        if (lan && upnp_map_udp(lp, lan, "SSBU online (lobby)", pub, sizeof pub, why, sizeof why)) { ext = lp; snprintf(g_map_how, sizeof g_map_how, "UPnP"); }
    }
    if (ext) {
        InterlockedExchange(&g_map_port, ext);
        InterlockedExchange(&g_map_state, 2);
        atexit(map_close);
        say("%s: the router forwards outside UDP port %u to this game's port %u: direct paths even where it changes ports", g_map_how, ext, lp);
    } else {
        InterlockedExchange(&g_map_state, 3);
        say("port mapping: the router answered neither PCP, NAT-PMP nor UPnP (off in the router?): direct paths only where it keeps ports");
    }
    while (ext && g_map_how[0] != 'U' && s != INVALID_SOCKET) {   /* (PCP / NAT-PMP: renewed at half its life) */
        Sleep(3600 * 1000);
        uint16_t again = pmp_ask(s, lp, ext, 7200, g_map_pcp);
        if (again && again != ext) { InterlockedExchange(&g_map_port, again); ext = again; }
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return 0;
}

static void map_start(void) {                            /* (client thread, LOBBY mode, joined) */
    if (g_map_state || !g_lan_port) return;
    uint32_t lobby = ntohl(g_lobby.sin_addr.s_addr);
    int nearby = g_loopback || test_loopback() || (lobby >> 24) == 127 || (lobby >> 24) == 10 || (lobby >> 20) == 0xAC1 || (lobby >> 16) == 0xC0A8;
    if (!g_upnp || nearby) { g_map_state = 3; return; }
    g_map_state = 1;
    g_map_local = g_lan_port;
    HANDLE th = CreateThread(NULL, 0, map_thread, NULL, 0, NULL);
    if (th) { SetThreadDescription(th, L"online port mapping"); CloseHandle(th); }
}
#endif
