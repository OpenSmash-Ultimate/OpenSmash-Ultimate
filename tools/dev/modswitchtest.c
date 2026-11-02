/* Offline checks of runtime/mod_switch.h (mods/mods.json read as the settings menu's Mods page reads
 * it: each mod's switch, in "order", titles, the code patches' setting and rule) and runtime/mod_diff.h
 * (what differs between two games' mod sets, in words). Built and run by tests/test_mod_switch.py (any
 * C compiler: no runtime). Prints "<n> checks, 0 failed" and exits 0 when every check passes. */
#include <stdio.h>
#include <string.h>
#include "../../runtime/mod_switch.h"
#include "../../runtime/mod_diff.h"

static int checks, failed;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failed++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Mod mod(const char *name, unsigned hash) {
    Mod m;
    snprintf(m.name, sizeof m.name, "%s", name);
    m.hash = hash;
    return m;
}

int main(void) {
    /* mods.json as tools/mods.py writes it (json.dump, indent 1): "order" first or last, other keys kept */
    const char *js =
        "{\n \"order\": [\n  \"Beta\",\n  \"Caf\\u00e9 \\\"pack\\\"\",\n  \"Alpha\"\n ],\n"
        " \"mods\": {\n  \"Alpha\": {\n   \"enabled\": true,\n   \"gamebanana\": 12345,\n   \"tags\": [1, {\"x\": [2, 3]}],\n   \"title\": \"The Alpha\"\n  },\n"
        "  \"Beta\": {\n   \"enabled\": false\n  },\n"
        "  \"Caf\\u00e9 \\\"pack\\\"\": {\n   \"title\": \"\\ud83d\\ude00 smile\",\n   \"enabled\": true\n  }\n },\n"
        " \"code_patches\": false\n}\n";
    ModSwitch m[8];
    int code = 7;
    int n = mod_switch_read(js, m, 8, &code);
    CHECK(n == 3, "three mods, not %d", n);
    CHECK(code == 0, "code_patches false: %d", code);
    if (n == 3) {
        CHECK(!strcmp(m[0].name, "Beta") && !m[0].on && !strcmp(m[0].title, "Beta"), "Beta first, off, its folder for a title: %s %d %s", m[0].name, m[0].on, m[0].title);
        CHECK(!strcmp(m[1].name, "Caf\xc3\xa9 \"pack\"") && m[1].on && !strcmp(m[1].title, "\xf0\x9f\x98\x80 smile"), "the escaped one: %s %d %s", m[1].name,
              m[1].on, m[1].title);
        CHECK(!strcmp(m[2].name, "Alpha") && m[2].on && !strcmp(m[2].title, "The Alpha"), "Alpha last, on, titled: %s %d %s", m[2].name, m[2].on, m[2].title);
        CHECK(!mod_switch_name_ok(m[1].name), "a name with a quote is not passed on a command line");
        CHECK(mod_switch_name_ok(m[2].name), "a plain name is");
    }
    n = mod_switch_read("{\"mods\": {\"A\": {\"enabled\": true}}, \"code_patches\": true}", m, 8, &code);
    CHECK(n == 1 && code == 1 && m[0].on, "no order, code patches on: %d %d", n, code);
    n = mod_switch_read("{\"order\": [], \"mods\": {}}", m, 8, &code);
    CHECK(n == 0 && code == -1, "none, code patches unset: %d %d", n, code);
    CHECK(mod_switch_read("not json", m, 8, &code) == -1, "not an object");
    CHECK(mod_switch_read("{\"mods\": {\"A\": {\"enabled\": tr", m, 8, &code) == -1, "cut short");
    n = mod_switch_read("{\"mods\": {\"A\": {}, \"B\": {}, \"C\": {}}}", m, 2, &code);
    CHECK(n == 2, "max: %d", n);
    /* the code patches' rule: the setting, else on while a mod runs */
    CHECK(mod_switch_code_patches(-1, 0) == 0 && mod_switch_code_patches(-1, 3) == 1, "auto");
    CHECK(mod_switch_code_patches(0, 3) == 0 && mod_switch_code_patches(1, 0) == 1, "set");

    /* what differs, in words */
    char out[600];
    Mod room[4] = {mod("Alpha", 1), mod("Beta", 2), mod(MOD_CODE_PATCHES, 9)}, mine[4] = {mod("beta", 3), mod("Gamma", 4)};
    mods_diff_text(room, 3, mine, 2, "the room", out, sizeof out);
    CHECK(!strcmp(out, "Code patches (ARCropolis): on in the room, off in yours (Mods page)\n"
                       "Mods you don't have: Alpha\n"
                       "Another copy of: Beta (yours differs from the room's)\n"
                       "Mods not in the room: Gamma (turn them off to join)"), "the diff:\n%s", out);
    mods_diff_text(room, 2, room, 2, "the room", out, sizeof out);
    CHECK(!out[0], "the same mods: nothing: %s", out);
    mods_list_marked(room, 3, mine, 2, out, sizeof out);
    CHECK(!strcmp(out, "Alpha (you don't have it), Beta (yours differs), ARCropolis code patches (off in yours)"), "marked: %s", out);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
