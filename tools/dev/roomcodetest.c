/* Offline checks of runtime/room_code.h: a private room's code made from an address and read back,
 * the forms a person may type, and what is not a code. Built and run by tests/test_room_code.py
 * (any C compiler: no runtime). Prints "<n> checks, 0 failed" and exits 0 when every check passes. */
#include <stdio.h>
#include <string.h>
#include "../../runtime/room_code.h"

static int checks, failed;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failed++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static const struct { uint32_t ip; uint16_t port; } addrs[] = {
        {0xC0A80114, 39420}, {0x5DB8D822, 39420}, {0x7F000001, 1}, {0xFFFFFFFF, 65535}, {0x01020304, 7701},
    };
    for (size_t i = 0; i < sizeof addrs / sizeof *addrs; i++) {
        char code[12];
        uint32_t ip = 0;
        uint16_t port = 0;
        room_code_of(addrs[i].ip, addrs[i].port, code);
        CHECK(strlen(code) == 11 && code[5] == '-', "%s: XXXXX-XXXXX", code);
        CHECK(room_code_read(code, &ip, &port) && ip == addrs[i].ip && port == addrs[i].port, "%s read back", code);
        char loose[16];   /* lower case, no dash, spaces, O and I, L as a person may type them */
        int k = 0;
        for (int j = 0; code[j]; j++) {
            char c = code[j];
            if (c == '-') { loose[k++] = ' '; continue; }
            loose[k++] = c == '0' ? 'o' : c == '1' ? 'l' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        loose[k] = 0;
        CHECK(room_code_read(loose, &ip, &port) && ip == addrs[i].ip && port == addrs[i].port, "\"%s\" read as %s", loose, code);
    }
    uint32_t ip;
    uint16_t port;
    static const char *const not_codes[] = {"192.168.1.20:39420", "smash.recomps.wtf", "ABCDE-FGHJ", "ABCDE-FGHJKM", "00000-00000", "ZZZZZ-ZZZZZ", "", "ABCDE-FGHJU"};
    for (size_t i = 0; i < sizeof not_codes / sizeof *not_codes; i++)
        CHECK(!room_code_read(not_codes[i], &ip, &port), "\"%s\" is no code", not_codes[i]);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
