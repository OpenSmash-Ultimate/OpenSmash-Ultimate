/* The settings folder (runtime/hle_config.c) and controllers.ini in either schema (hle_pad.c), on
 * made-up files: tests/test_config_folder.py lays out a folder, runs this in it and looks at what
 * it left. One thing a run, as the game does each once a start:
 *
 *   configtest paths              the folder made (the first time), then each file's path
 *   configtest load               controllers.ini read as the game reads it (written when missing),
 *                                 then what it says (as dump)
 *   configtest dump <file>        a controllers.ini as the game reads it: every setting it ends up with
 *   configtest convert <from> <to>  an ssbu-recomp controllers.ini rewritten in this port's terms
 *   configtest set <file> <section|-> <key> <value|->   one change as the settings menu makes it
 *
 * Built with runtime/hle_config.c and hle_settings.c (and the platform layer outside Windows):
 *   clang -O1 -I runtime/macos/include -I runtime/linux/include -I runtime tools/dev/configtest.c \
 *       runtime/hle_config.c runtime/hle_settings.c runtime/linux/win32.c runtime/macos/memory.c \
 *       runtime/macos/platform.c -o configtest            (macOS; Linux: -lpthread -lm for the last two)
 * Exit code 0 when it did what it was asked. */
#include "../../runtime/hle_pad.c"
#include "../../runtime/hle_padusb.c"

/* (gpu_display.c's, which is not built here: display.ini's defaults, made up) */
const char *display_default_ini(void) {
    return "; display.ini (configtest's defaults)\n"
           "window = windowed\n"
           "width = 1280\n"
           "height = 720\n"
           "pacing = clock\n"
           "overlay = 1\n"
           "render_scale = 1\n";
}

static const char *source_name(Source s) {
    if (s.kind == SRC_TRIGGER) return s.index == RA_LEFT_TRIGGER ? "lefttrigger" : "righttrigger";
    return s.kind == SRC_BUTTON && s.index < RB_COUNT ? RAW_BUTTON_NAMES[s.index] : "none";
}

static void dump_dev(const DevCfg *d) {
    printf("[device %s] layout=%d style=%d deadzone=%d rumble=%d trigger_threshold=%d\n", d->match[0] ? d->match : "default",
           d->layout, d->style, d->deadzone, d->rumble, d->trigger);
    for (int b = 0; b < 16; b++) {
        if (!d->has_map[b]) continue;
        printf("  %s =", NPAD_BUTTON_NAMES[b]);
        if (!d->nmap[b]) printf(" none");
        for (int i = 0; i < d->nmap[b]; i++) printf("%s %s", i ? "," : "", source_name(d->map[b][i]));
        printf("\n");
    }
}

/* What the settings are now: the general ones, the players kept, then each [device]. */
static void dump(void) {
    printf("keyboard_player=%d background=%d rumble=%d\n", cfg.keyboard_slot + 1, cfg.background, cfg.rumble);
    for (int p = 0; p < PAD_SLOTS; p++)
        if (cfg.pin[p][0]) printf("player %d = %s\n", p + 1, cfg.pin[p]);
    dump_dev(&cfg.def);
    for (int i = 0; i < cfg.ndev; i++) dump_dev(&cfg.dev[i]);
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "";
    config_defaults();
    if (!strcmp(what, "paths")) {
        static const char *const NAMES[CONFIG_FILES] = {"display", "controllers", "launcher", "online_server", "online_name", "online_direct",
                                                        "online_protocol"};
        for (int f = 0; f < CONFIG_FILES; f++) printf("%s: %s\n", NAMES[f], config_path((enum ConfigFile)f));
        return 0;
    }
    if (!strcmp(what, "load")) {
        config_load();
        printf("path: %s\n", cfg.path);
        dump();
        return 0;
    }
    if (!strcmp(what, "dump") && argc == 3) {
        FILE *f = fopen(argv[2], "r");
        if (!f) return 2;
        config_parse(f);
        fclose(f);
        dump();
        return 0;
    }
    if (!strcmp(what, "convert") && argc == 4) return pad_ini_convert(argv[2], argv[3]) ? 0 : 2;
    if (!strcmp(what, "set") && argc == 6)
        return settings_ini_set(argv[2], strcmp(argv[3], "-") ? argv[3] : NULL, argv[4], strcmp(argv[5], "-") ? argv[5] : NULL) ? 0 : 2;
    fprintf(stderr, "usage: configtest paths | load | dump <file> | convert <from> <to> | set <file> <section|-> <key> <value|->\n");
    return 3;
}
