/* A private room's code (net_relay.c): the host's IPv4 address and UDP port, 6 bytes, in Crockford
 * base32 - 10 characters, shown as XXXXX-XXXXX - so a friend joins by typing the code instead of
 * an address. Reading one ignores '-', spaces and case, and takes O for 0 and I or L for 1.
 * Kept apart, without the runtime, so tools/dev/roomcodetest.c can test it. (From ssbu-recomp.) */
#pragma once
#include <stdint.h>
#include <string.h>

static const char ROOM_CODE_DIGITS[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* out: 12 bytes. ip in host order. */
static void room_code_of(uint32_t ip, uint16_t port, char *out) {
    uint64_t v = (uint64_t)ip << 16 | port;
    char t[10];
    for (int i = 9; i >= 0; i--) { t[i] = ROOM_CODE_DIGITS[v & 31]; v >>= 5; }
    memcpy(out, t, 5);
    out[5] = '-';
    memcpy(out + 6, t + 5, 5);
    out[11] = 0;
}

/* 1 and the address (host order) when s is a code: 10 digits, nothing else but '-' and spaces;
 * else 0 (an address or a name, for the caller to look up). */
static int room_code_read(const char *s, uint32_t *ip, uint16_t *port) {
    uint64_t v = 0;
    int n = 0;
    for (; s && *s; s++) {
        int c = *s >= 'a' && *s <= 'z' ? *s - 32 : *s, d;
        if (c == '-' || c == ' ') continue;
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char *p = c ? strchr(ROOM_CODE_DIGITS, c) : NULL;
        if (!p) return 0;
        d = (int)(p - ROOM_CODE_DIGITS);
        v = v << 5 | (uint64_t)d;
        if (++n > 10) return 0;
    }
    if (n != 10 || (v >> 48) || !(v & 0xFFFF)) return 0;   /* (50 bits: the top two zero; port 0 is no room) */
    *ip = (uint32_t)(v >> 16);
    *port = (uint16_t)v;
    return 1;
}
