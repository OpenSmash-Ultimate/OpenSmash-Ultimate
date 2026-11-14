/* Exercise the real frame handlers with a synthetic game/controller, without game files. */
#include "../../runtime/hle_mod_refresh.c"
#include "../../runtime/hle_mod_material.c"

static char test_build_id[] = "21450C647B8C5940ED5B68218EA128FD3C5D597D";
const RtModule rt_module_main = {.base = 0x7100000000ull, .build_id = test_build_id};

const char *rt_data_dir = ".";
static const char *preference;
static int mode, symbol_available = 1, calls, blocked;
static u64 buttons;
static u8 *arena;
static int material_calls;
static int setter_drops;
static int render_calls, render_context_ok = 1;
u64 rt_native(GuestFn *fn, const char *name) { return 0; }
u64 rt_hook_function(u64 target, u64 replace) { return 0; }
GuestFn *rt_lookup(u64 address) { return NULL; }
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
void rt_call(Cpu *c, u64 addr) {
    if (addr == rt_module_main.base + 0x38a9ca0) {
        u64 node = c->x[1] - 0x10, object = c->x[0];
        if (mm_u32(G2H(c->x[2])) != 5 || mm_u32((u8 *)G2H(node) + 0x20) != 5) abort();
        u32 id = mm_u32((u8 *)G2H(node) + 0x18);
        u64 target = node + 0x28;
        if (c->x[3] == 1 && id == 160 && mm_u64((u8 *)G2H(object) + 0x48))
            target = mm_u64((u8 *)G2H(object) + 0x48) + 8;
        if (!setter_drops) memcpy(G2H(target), (u8 *)G2H(c->x[2]) + 8, 16);
        u64 *dirty = (u64 *)((u8 *)G2H(object) + 0xd8 + (id / 64) * 8);
        *dirty |= 1ull << (id & 63); material_calls++;
    } else {
        if (addr != 123) abort(); calls++;
    }
    memset(c, 0xaa, sizeof *c); c->x[0] = mode;
}
int frame_hook_add(int p, int o, FrameHookFn *b, FrameHookFn *a, void *ctx) { return 1; }
int menu_blocks_input(void) { return blocked; }
int pad_get(u32 slot, PadState *out) { memset(out, 0, sizeof *out); out->buttons = buttons; return slot == 0; }
void kernel_memory_hold(void) {}
void kernel_memory_release(void) {}
u32 kernel_writable_ranges(KernelRange *out, u32 max) {
    if (!arena) return 0;
    out[0] = (KernelRange){0x6000000000ull, 0x6000010000ull, 0}; return 1;
}
int rb_page_cold(const void *p) { return 0; }

static Cpu cpu, saved;
static float setter_seen[4];
static void setter_stub(Cpu *c) { memcpy(setter_seen, (u8 *)G2H(c->x[2]) + 8, 16); }
/* The game's per-frame baseline copy (36a8320) must keep the edited value. */
static int reset_setter(u64 object, u32 id, const float *value, u64 caller) {
    u8 *descriptor = arena + 0x7000, *variant = arena + 0x7100;
    memset(descriptor, 0, 16); memcpy(descriptor + 8, &id, 4);
    memset(variant, 0, 24); variant[0] = 5; memcpy(variant + 8, value, 16);
    Cpu c = {0}; c.x[0] = object; c.x[1] = 0x6000007000ull; c.x[2] = 0x6000007100ull; c.x[30] = caller;
    material_setter(&c); return 0;
}
static void render_original(Cpu *c) {
    Cpu expected = saved; expected.x[0] = c->x[0];
    if (memcmp(c, &expected, sizeof *c)) render_context_ok = 0;
    render_calls++;
}
static void render_tick(u64 object) {
    cpu = saved; cpu.x[0] = object;
    material_render(&cpu);
    Cpu expected = saved; expected.x[0] = object;
    if (memcmp(&cpu, &expected, sizeof cpu)) abort();
}
static void tick(u64 value) {
    buttons = value;
    mod_refresh_before(&cpu, NULL, NULL);
    if (memcmp(&cpu, &saved, sizeof cpu)) abort();
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); return 1; } } while (0)
static void short_string(u8 *p, const char *text) {
    size_t n = strlen(text); if (n > 22) abort();
    memset(p, 0, 24); p[0] = (u8)(n * 2); memcpy(p + 1, text, n);
}
static void material_fixture(u32 index, const char *label) {
    u8 *object = arena + 0x200 + 0x200 * index, *node = arena + 0x1000 + 0x80 * index;
    u64 vptr = rt_module_main.base + 0x526a808, node_addr = 0x6000001000ull + 0x80 * index;
    memset(object, 0, 0x138); memset(node, 0, 0x38);
    memcpy(object, &vptr, 8); memcpy(object + 0x20, &node_addr, 8);
    short_string(object + 0x108, label);
    const char *shader = "SFX_PBS_012000080800826b_opaque";
    u64 shader_length = strlen(shader), shader_capacity = 65, shader_address = 0x6000002000ull + 0x100 * index;
    memcpy(object + 0x120, &shader_capacity, 8); memcpy(object + 0x128, &shader_length, 8);
    memcpy(object + 0x130, &shader_address, 8); memcpy(G2H(shader_address), shader, shader_length + 1);
    u32 id = 160, type = 5; float vector[] = {1, 1, 1, 1};
    memcpy(node + 0x18, &id, 4); memcpy(node + 0x20, &type, 4); memcpy(node + 0x28, vector, 16);
    u64 cached = 0x6000003000ull + 0x40 * index;
    memcpy(object + 0x48, &cached, 8); memset(G2H(cached), 0, 24);
    memcpy(G2H(cached), &type, 4); memcpy((u8 *)G2H(cached) + 8, vector, 16);
    u8 *second = arena + 0x1400 + 0x80 * index;
    u64 second_addr = 0x6000001400ull + 0x80 * index;
    memcpy(node, &second_addr, 8); memset(second, 0, 0x38);
    id = 161; memcpy(second + 0x18, &id, 4); memcpy(second + 0x20, &type, 4);
    /* Animation/gameplay has changed a different vector since loading. That
     * must not prevent updating the vectors actually edited on disk. */
    u8 *third = arena + 0x1800 + 0x80 * index; u64 third_addr = 0x6000001800ull + 0x80 * index;
    memcpy(second, &third_addr, 8); memset(third, 0, 0x38);
    id = 162; float animated[] = {5, 6, 7, 8};
    memcpy(third + 0x18, &id, 4); memcpy(third + 0x20, &type, 4); memcpy(third + 0x28, animated, 16);

}
static Snap pending_snapshot(const u8 *old, u32 old_size, const u8 *next, u32 new_size, u64 request) {
    Snap s = {.size = old_size, .capacity = old_size, .material_request = request, .material_size = new_size};
    s.bytes = malloc(old_size); memcpy(s.bytes, old, old_size);
    s.material_bytes = malloc(new_size); memcpy(s.material_bytes, next, new_size);
    s.material_mtime.dwLowDateTime = 42;
    return s;
}
static int material_index_checks(void) {
    u64 seen[32768] = {0}, a = 0, b = 0, third = 0x600000f000ull;
    for (u64 offset = 0x200; offset < 0xe000; offset += 16) {
        u64 object = 0x6000000000ull + offset; u32 bucket = live_bucket(object);
        if (seen[bucket]) { a = seen[bucket]; b = object; break; }
        seen[bucket] = object;
    }
    CHECK(a && b && a != b && live_bucket(a) == live_bucket(b));
    remember_material(a); remember_material(a); CHECK(live_count == 1);
    remember_material(b); remember_material(third);
    CHECK(live_index(a) == 0 && live_index(b) == 1 && live_index(third) == 2);
    forget_material(a); /* delete collision-chain tail; move last entry */
    CHECK(live_index(a) == ~0u && live_index(b) == 1 && live_index(third) == 0);
    forget_material(b); CHECK(live_index(b) == ~0u && live_index(third) == 0);
    remember_material(a); remember_material(b);
    forget_material(third); /* moving a collided entry must repair its incoming link */
    CHECK(live_index(third) == ~0u && live_index(a) == 1 && live_index(b) == 0);
    forget_material(a); forget_material(b); CHECK(live_count == 0);
    for (u32 i = 0; i < 32768; i++) CHECK(live_heads[i] == 0);
    return 0;
}
static int material_checks(const char *old_path, const char *new_path) {
    u32 old_size, new_size; u8 *old = read_all(old_path, &old_size, NULL), *next = read_all(new_path, &new_size, NULL);
    CHECK(old && next);
    arena = VirtualAlloc((void *)0x6000000000ull, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(arena == G2H(0x6000000000ull));
    CHECK(material_index_checks() == 0);
    char variant[128];
    CHECK(shader_normal_variant("SFX_PBS_010000080800826b_opaque", variant, sizeof variant));
    CHECK(!strcmp(variant, "SFX_PBS_012000080800826b_opaque"));
    CHECK(shader_identity("SFX_PBS_010000080800826b_opaque", variant));
    CHECK(!shader_normal_variant(variant, variant, sizeof variant));
    CHECK(!shader_identity("SFX_PBS_010000080800826b_opaque", "shadow_cast_only"));
    CHECK(!shader_identity("SFX_PBS_010000080800826b_opaque", "SFX_PBS_014000080800826b_opaque"));
    CHECK(!shader_identity("SFX_PBS_010000080800826b_opaque", "SFX_PBS_012000080800826b_sort"));
    CHECK(!shader_normal_variant("SFX_PBS_010000080800826z_opaque", variant, sizeof variant));
    material_fixture(0, "goku_test"); material_fixture(1, "goku_test"); material_fixture(2, "goku_test");
    short_string(arena + 0x600 + 0x120, "shadow_cast_only");
    CHECK(!guest_shader(0x6000000720ull, "SFX_PBS_010000080800826b_opaque"));
    /* Same names, but a mixture of old and new values: this object is not our
     * original material signature and must not have its first vector changed. */
    float foreign[] = {.4f, .5f, .6f, 1}; memcpy(arena + 0x1528, foreign, 16);
    tracking_ready = 1;
    short_string(arena + 0x308, "constructing");
    for (u32 i = 0; i < 3; i++) remember_material(0x6000000200ull + 0x200 * i);
    short_string(arena + 0x308, "goku_test"); /* caller finishes naming after base ctor */
    memset(&cpu, 0x35, sizeof cpu); cpu.sp = 0x6000008000ull; saved = cpu;
    material_render_original = render_original;
    memset(arena + 0x7f00, 0x43, 0x100);
    material_deadline = GetTickCount64() - 1;
    CHECK(!readable(0x6000000200ull, 0x138)); material_deadline = 0;
    ModMaterialFile files[] = {{old, old_size}, {old, old_size}};
    u32 updated; u64 request; char reason[200];
    CHECK(!mod_material_refresh(&cpu, files, 2, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(material_calls == 0 && strstr(reason, "another watched asset"));
    /* A second asset already naming the derived normal shader is equally
     * ambiguous: normalization must not weaken the ownership guard. */
    u8 *other = malloc(old_size); memcpy(other, old, old_size);
    File other_file; Entry other_entry;
    CHECK(file_read(other, old_size, &other_file) && entry_read(other_file, 0, &other_entry));
    ((char *)other_entry.shader)[10] = '2'; files[1] = (ModMaterialFile){other, old_size};
    CHECK(!mod_material_refresh(&cpu, files, 2, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(strstr(reason, "another watched asset") && material_calls == 0); free(other);
    test_build_id[0] = '0';
    CHECK(!mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(material_calls == 0 && strstr(reason, "game version")); test_build_id[0] = '2';
    /* Cached objects that have never reached render-update are not targets. */
    CHECK(!mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(material_calls == 0 && pending_count == 0);
    for (u32 i = 0; i < 3; i++) render_tick(0x6000000200ull + 0x200 * i);
    cpu = saved;
    CHECK(mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(updated == 4 && material_calls == 0 && pending_count == 4 && !memcmp(&cpu, &saved, sizeof cpu));
    CHECK(strstr(reason, "1 inactive material"));
    Snap transaction = pending_snapshot(old, old_size, next, new_size, request);
    u8 *old_snapshot = transaction.bytes;
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_PENDING);
    CHECK(transaction.bytes == old_snapshot && transaction.size == old_size && transaction.mtime.dwLowDateTime == 0);
    float original_vector[] = {1, 1, 1, 1}; CHECK(!memcmp(arena + 0x1028, original_vector, 16));
    File next_file; Entry next_entry;
    CHECK(file_read(next, new_size, &next_file) && entry_read(next_file, 0, &next_entry));
    const Attr *next_vector = attribute(&next_entry, 160);
    u8 vector_was[16]; memcpy(vector_was, next_vector->value.p, 16);
    memset((void *)next_vector->value.p, 0xee, 16); /* queued values own their bytes */
    render_tick(0x6000000200ull); CHECK(material_calls == 2 && pending_count == 2);
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_PENDING);
    CHECK(transaction.bytes == old_snapshot);
    render_tick(0x6000000400ull); CHECK(material_calls == 4 && pending_count == 0);
    memcpy((void *)next_vector->value.p, vector_was, 16);
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_APPLIED);
    CHECK(!transaction.material_request && !transaction.material_bytes && transaction.size == new_size);
    CHECK(transaction.mtime.dwLowDateTime == 42 && !memcmp(transaction.bytes, next, new_size));
    free(transaction.bytes);
    CHECK(render_context_ok);
    render_tick(0x6000000200ull); CHECK(material_calls == 4); /* one shot, no permanent override */
    float expected[] = {.2f, .3f, .4f, 1}, untouched[] = {1, 1, 1, 1};
    CHECK(!memcmp(arena + 0x3008, expected, 16) && !memcmp(arena + 0x3048, expected, 16));
    CHECK(!memcmp(arena + 0x1028, untouched, 16) && !memcmp(arena + 0x10a8, untouched, 16));
    CHECK(!memcmp(arena + 0x1128, untouched, 16));
    float animated[] = {5, 6, 7, 8}; CHECK(!memcmp(arena + 0x1828, animated, 16));
    CHECK(mm_u64(arena + 0x200 + 0xe8) == ((1ull << 32) | (1ull << 33)));
    for (u32 i = 0; i < 0x100; i++) CHECK(arena[0x7f00 + i] == 0x43);
    setter_original = setter_stub;
    u64 reset = rt_module_main.base + 0x36a8574;
    CHECK(override_count == 4);
    reset_setter(0x6000000200ull, 160, untouched, reset); CHECK(!memcmp(setter_seen, expected, 16));
    reset_setter(0x6000000200ull, 160, animated, reset); CHECK(!memcmp(setter_seen, animated, 16));
    reset_setter(0x6000000200ull, 160, untouched, 0); CHECK(!memcmp(setter_seen, untouched, 16));
    reset_setter(0x6000000200ull, 162, untouched, reset); CHECK(!memcmp(setter_seen, untouched, 16));
    reset_setter(0x6000000600ull, 160, untouched, reset); CHECK(!memcmp(setter_seen, untouched, 16));
    /* Reverting to the original value drops the override. */
    remember_override(0x6000000200ull, 160, (const u8 *)untouched, (const u8 *)untouched);
    CHECK(override_count == 3);
    reset_setter(0x6000000200ull, 160, untouched, reset); CHECK(!memcmp(setter_seen, untouched, 16));
    remember_override(0x6000000200ull, 160, (const u8 *)untouched, (const u8 *)expected);
    CHECK(override_count == 4);
    /* A property replaced after queueing cannot be written through a stale pointer. */
    for (u32 i = 0; i < 3; i++) material_fixture(i, "goku_test");
    cpu = saved;
    CHECK(mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(pending_count == 6);
    transaction = pending_snapshot(old, old_size, next, new_size, request); old_snapshot = transaction.bytes;
    u64 missing = 0; memcpy(arena + 0x220, &missing, 8);
    render_tick(0x6000000200ull); CHECK(material_calls == 4 && pending_count == 0);
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_FAILED);
    CHECK(transaction.bytes == old_snapshot && transaction.size == old_size && !transaction.material_partial);
    CHECK(transaction.mtime.dwLowDateTime == 0 && !transaction.material_bytes); free(transaction.bytes);
    /* Destruction cancels requests even if the old allocation retains its signature. */
    for (u32 i = 0; i < 3; i++) material_fixture(i, "goku_test");
    cpu = saved;
    CHECK(mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    transaction = pending_snapshot(old, old_size, next, new_size, request); old_snapshot = transaction.bytes;
    render_tick(0x6000000200ull); CHECK(material_calls == 6 && pending_count == 4);
    forget_material(0x6000000400ull); CHECK(pending_count == 0);
    CHECK(override_count == 2); /* destruction drops the object's overrides */
    reset_setter(0x6000000400ull, 160, untouched, reset); CHECK(!memcmp(setter_seen, untouched, 16));
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_PARTIAL);
    CHECK(transaction.bytes == old_snapshot && transaction.size == old_size && transaction.material_partial);
    CHECK(strstr(reason, "restart the game")); free(transaction.bytes);
    render_tick(0x6000000400ull); CHECK(material_calls == 6);
    /* Freed objects can retain their entire old signature: lifetime tracking must exclude them. */
    for (u32 i = 0; i < 3; i++) { forget_material(0x6000000200ull + 0x200 * i); material_fixture(i, "goku_test"); }
    material_calls = 0;
    CHECK(!mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(material_calls == 0 && live_count == 0 && pending_count == 0);
    /* A rendered identity whose values no longer match cannot be treated as
     * an inactive entry and silently advanced to the next snapshot. */
    material_fixture(0, "goku_test"); remember_material(0x6000000200ull);
    render_tick(0x6000000200ull); cpu = saved;
    float changed_live[] = {9, 9, 9, 9}; memcpy(arena + 0x3008, changed_live, 16);
    CHECK(!mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(strstr(reason, "rendered material values differ") && pending_count == 0 && material_calls == 0);
    forget_material(0x6000000200ull);
    /* A setter that returns without changing the effective cached vector
     * must not acknowledge a successful revision. */
    material_fixture(0, "goku_test"); remember_material(0x6000000200ull);
    render_tick(0x6000000200ull); cpu = saved;
    CHECK(mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    transaction = pending_snapshot(old, old_size, next, new_size, request); old_snapshot = transaction.bytes;
    setter_drops = 1; render_tick(0x6000000200ull); setter_drops = 0;
    CHECK(material_complete(&transaction, reason, sizeof reason) == MOD_MATERIAL_FAILED);
    CHECK(transaction.bytes == old_snapshot && transaction.size == old_size);
    CHECK(!memcmp(arena + 0x3008, untouched, 16)); free(transaction.bytes);
    forget_material(0x6000000200ull);
    /* A malformed relative pointer cannot reach guest calls or partially apply. */
    memset(next + 24, 0xff, 8); material_calls = 0;
    CHECK(!mod_material_refresh(&cpu, files, 1, 0, next, new_size, &updated, &request, reason, sizeof reason));
    CHECK(material_calls == 0);
    VirtualFree(arena, 0, MEM_RELEASE); arena = NULL; free(old); free(next);
    puts("PASS live material updates, render queue, invalidation, lifetime, dirty state and guest context"); return 0;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--materials")) return material_checks(argv[2], argv[3]);
    /* Every requested format enters the same buffer refresh path. Validate replacement,
     * clearing a smaller export's tail, and rejecting an export that cannot fit. */
    const char *assets[] = { "model.numshb", "model.nusktb", "model.nuhlpb", "model.numatb",
        "model.numdlb", "model.numshexb", "model.nutexb", "motion.nuanmb", "texture.btnx",
        "TEXTURE.BTNX", "MODEL.NUMSHEXB" };
    for (unsigned i = 0; i < sizeof assets / sizeof assets[0]; i++) {
        CHECK(file_kind(assets[i]) >= 0);
        u8 old[16], replacement[16], memory[32];
        memset(old, 0x12, sizeof old); memset(replacement, 0x34, sizeof replacement);
        memset(memory, 0xff, sizeof memory); memcpy(memory + 8, old, sizeof old);
        CHECK(splice_span(memory, sizeof memory, old, sizeof old, replacement, 12, 16, 1) == 1);
        CHECK(!memcmp(memory + 8, replacement, 12));
        CHECK(memory[20] == 0 && memory[23] == 0 && memory[7] == 0xff && memory[24] == 0xff);
        memcpy(memory + 8, old, sizeof old);
        CHECK(splice_span(memory, sizeof memory, old, sizeof old, replacement, 16, 12, 1) == 0);
        CHECK(!memcmp(memory + 8, old, sizeof old));
    }
    CHECK(file_kind("plugin.nro") == -1 && file_kind("texture.btnx.bak") == -1);
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
