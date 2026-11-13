/* Exercise the real frame handlers with a synthetic game/controller, without game files. */
#include "../../runtime/hle_mod_refresh.c"

const char *rt_data_dir = ".";
static const char *preference;
static int mode, symbol_available = 1, calls, blocked;
static u64 buttons;
int settings_ini_get(const char *path, const char *section, const char *key, char *out, int size) {
    if (strcmp(section, "opensmash") || strcmp(key, "hot_reload_mod_assets")) abort();
    if (!preference) return 0;
    snprintf(out, size, "%s", preference); return 1;
}
const char *config_path(enum ConfigFile file) { if (file != CONFIG_LAUNCHER) abort(); return "launcher.ini"; }
u64 rt_symbol(const char *name) {
    if (strcmp(name, "_ZN3app9smashball16is_training_modeEv")) abort();
    return symbol_available ? 123 : 0;
}
void rt_call(Cpu *c, u64 addr) { if (addr != 123) abort(); calls++; memset(c, 0xaa, sizeof *c); c->x[0] = mode; }
int frame_hook_add(int p, int o, FrameHookFn *b, FrameHookFn *a, void *ctx) { return 1; }
int menu_blocks_input(void) { return blocked; }
int pad_get(u32 slot, PadState *out) { memset(out, 0, sizeof *out); out->buttons = buttons; return slot == 0; }
void kernel_memory_hold(void) {}
void kernel_memory_release(void) {}
u32 kernel_writable_ranges(KernelRange *out, u32 max) { return 0; }
int rb_page_cold(const void *p) { return 0; }

static Cpu cpu, saved;
static void tick(u64 value) {
    buttons = value;
    mod_refresh_before(&cpu, NULL, NULL);
    if (memcmp(&cpu, &saved, sizeof cpu)) abort();
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(int argc, char **argv) {
    preference = argc > 1 ? argv[1] : NULL;
    memset(&cpu, 0x35, sizeof cpu); saved = cpu;
    if (!preference || strcmp(preference, "1")) {
        mod_refresh_boot(&cpu, NULL, NULL); tick(RESET_COMBO);
        CHECK(snap_started == 0 && calls == 0);
    } else {
        snap_started = 1; /* avoid filesystem work in the synthetic fixture */
        snap_budget = 1; /* refresh reports not-ready, so the Python test can count invocations */
        mode = 0; tick(RESET_COMBO); CHECK(calls == 1);
        mode = 1; tick(RESET_COMBO); CHECK(calls == 1); /* a held combo is not a second press */
        tick(0); symbol_available = 0; tick(RESET_COMBO); CHECK(calls == 1);
        tick(0); symbol_available = 1; blocked = 1; tick(RESET_COMBO); CHECK(calls == 1);
        blocked = 0; tick(0); tick(RESET_COMBO); CHECK(calls == 2);
        tick(RESET_COMBO); CHECK(calls == 2);
    }
    puts("PASS mod refresh policy and guest registers"); return 0;
}
