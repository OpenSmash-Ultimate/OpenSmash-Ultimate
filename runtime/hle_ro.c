/* nn::ro - modules the game loads at run time (NROs), natively.
 *
 * The game reads each NRO out of its archive itself and hands the image to LoadModule. Its
 * code was recompiled ahead of time for a fixed address (tools/recomp.py), so loading means
 * mapping that module and linking it (rt_load_nro). An NRO that has not been recompiled yet
 * is saved to <data dir>/../nro/<build id>.nro for the next build to pick up, and reported
 * as loaded so that the game goes on to hand over the rest. */
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <direct.h>
#endif
#include <stdio.h>

#include "host.h"

typedef struct Module { u64 object; u32 state; u32 pad; u64 image, buffer; } Module;   /* nn::ro::Module */
enum { STATE_LOADED = 1 };

static u32 harvested;

static void harvest(u64 image) {
    char dir[1024], path[1200], id[65];
    for (int i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02X", *P(u8, image + 0x40 + i));
    for (int n = 64; n > 1 && id[n - 1] == '0'; n--) id[n - 1] = 0;
    snprintf(dir, sizeof dir, "%s/../nro", rt_data_dir);
    _mkdir(dir);
    snprintf(path, sizeof path, "%s/%s.nro", dir, id);
    u32 size = *P(u32, image + 0x18);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(P(void, image), 1, size, f) != size) rt_fatal("cannot save %s", path);
    fclose(f);
    harvested++;
    fprintf(stderr, "[ro] saved NRO %s (%u bytes) - not recompiled yet (%u so far)\n", id, size, harvested);
}

HLE(_ZN2nn2ro10InitializeEv) { c->x[0] = 0; }
HLE(_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv) { c->x[0] = 0; }
HLE(_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKvj) { c->x[0] = 0; }
HLE(_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE) { c->x[0] = 0; }

/* GetBufferSize(size_t *size, const void *image): the module's .bss, which the caller provides. */
HLE(_ZN2nn2ro13GetBufferSizeEPmPKv) {
    *P(u64, c->x[0]) = (*P(u32, c->x[1] + 0x38) + 0xFFFull) & ~0xFFFull;
    c->x[0] = 0;
}

/* LoadModule(Module *module, const void *image, void *buffer, size_t buffer_size, int bind_flag):
 * through LoadModuleInternal's dispatch entry once plugins may hook it (hle_rtld.c), which comes
 * back here (ro_load_module_native). The Module gets rtld's ModuleObject when there is one. */
int rtld_load_module(Cpu *c);
u64 rtld_object(u64 base);
void ro_load_module_native(Cpu *c);
HLE(_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi) {
    if (!rtld_load_module(c)) ro_load_module_native(c);
}

void skyline_mods_gate(const char *what);   /* (hle_skyline.c) */
void ro_load_module_native(Cpu *c) {
    skyline_mods_gate("a module load");
    u64 module = c->x[0], image = c->x[1], buffer = c->x[2];
    if (*P(u32, image + 0x10) != 0x304F524E) { c->x[0] = RESULT(22, 4); return; }   /* not "NRO0" */
    u64 base = rt_load_nro(c, image);
    if (!base) {
        harvest(image);
        base = image;
    }
    u64 object = rtld_object(base);
    *P(Module, module) = (Module){object ? object : base, STATE_LOADED, 0, image, buffer};
    c->x[0] = 0;
}

HLE(_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE) {   /* the code stays mapped: nothing reuses the address */
    P(Module, c->x[0])->state = 0;
    c->x[0] = 0;
}
