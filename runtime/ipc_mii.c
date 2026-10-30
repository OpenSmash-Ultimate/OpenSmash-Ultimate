/* The Mii database (mii:e / mii:u), natively.
 *
 * The console keeps the user's Miis in a system save; here they are the files in the miis
 * folder, one CharInfo (0x58 bytes) each - tools/miis.py imports them from an emulator's Mii
 * database, Mii Studio or a 3DS / Wii U Mii file - read once, in file-name order, up to the
 * console's 100. After them come the six built-in default Miis. BuildDefault/BuildRandom
 * produce characters the way the service does - as a CharInfo (layout taken from the SDK's
 * own CharInfoAccessor getters). */
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

#define SOURCE_DATABASE 1               /* (SourceFlag: which Miis a Get asks for) */
#define SOURCE_DEFAULT  2
#define ELEMENT_DATABASE 0              /* (Source: where a CharInfoElement's Mii is from) */
#define ELEMENT_DEFAULT  1
#define DEFAULT_COUNT   6
#define DATABASE_MAX    100
#define MIIS_DIR        "miis"

typedef struct CharInfo {
    u8 create_id[16];
    u16 nickname[11];
    u8 font_region, favorite_color, gender, height, build, type, region_move;
    u8 faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    u8 hair_type, hair_color, hair_flip;
    u8 eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    u8 eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    u8 nose_type, nose_scale, nose_y;
    u8 mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    u8 beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    u8 glass_type, glass_color, glass_scale, glass_y;
    u8 mole_type, mole_scale, mole_x, mole_y;
    u8 padding;
} CharInfo;
_Static_assert(sizeof(CharInfo) == 0x58, "CharInfo");
_Static_assert(offsetof(CharInfo, font_region) == 0x26 && offsetof(CharInfo, mole_y) == 0x56, "CharInfo layout");

static u32 random32(void) { unsigned v; rand_s(&v); return v; }

/* A plain, valid Mii: the default face with a given gender and favourite colour. */
static CharInfo make(int gender, int color) {
    CharInfo m = {
        .favorite_color = (u8)color, .gender = (u8)gender, .height = 64, .build = 64,
        .hair_type = gender ? 12 : 33, .hair_color = 1,
        .eye_type = gender ? 4 : 2, .eye_scale = 4, .eye_aspect = 3, .eye_rotate = gender ? 3 : 4, .eye_x = 2, .eye_y = 12,
        .eyebrow_type = gender ? 0 : 6, .eyebrow_color = 1, .eyebrow_scale = 4, .eyebrow_aspect = 3,
        .eyebrow_rotate = 6, .eyebrow_x = 2, .eyebrow_y = 10,
        .nose_type = 1, .nose_scale = 4, .nose_y = 9,
        .mouth_type = gender ? 1 : 23, .mouth_scale = 4, .mouth_aspect = 3, .mouth_y = 13,
        .mustache_scale = 4, .mustache_y = 10, .glass_scale = 4, .glass_y = 10,
        .mole_scale = 4, .mole_x = 2, .mole_y = 20,
    };
    static const char name[] = "no name";
    for (int i = 0; name[i]; i++) m.nickname[i] = (u16)name[i];
    return m;
}

static void new_id(CharInfo *m) {
    for (int i = 0; i < 16; i += 4) { u32 r = random32(); memcpy(m->create_id + i, &r, 4); }
    m->create_id[6] = (m->create_id[6] & 0x0F) | 0x40;   /* a version-4 UUID, as the console makes */
    m->create_id[8] = (m->create_id[8] & 0x3F) | 0x80;
}

static CharInfo build_default(u32 index) {
    CharInfo m = make(index >= 3, (int)(index % DEFAULT_COUNT) * 2);
    memset(m.create_id, 0, 16);
    m.create_id[0] = 0xD0 + (u8)index;                   /* fixed ids, so the defaults are stable */
    m.create_id[6] = 0x40; m.create_id[8] = 0x80;
    return m;
}

static CharInfo build_random(u32 age, u32 gender, u32 race) {
    (void)age; (void)race;
    CharInfo m = make(gender < 2 ? (int)gender : (int)(random32() & 1), (int)(random32() % 12));
    m.faceline_type = (u8)(random32() % 12);
    m.faceline_color = (u8)(random32() % 6);
    m.hair_type = (u8)(random32() % 132);
    m.hair_color = (u8)(random32() % 8);
    m.eye_type = (u8)(random32() % 60);
    m.eye_color = (u8)(random32() % 6);
    m.eyebrow_type = (u8)(random32() % 24);
    m.nose_type = (u8)(random32() % 18);
    m.mouth_type = (u8)(random32() % 36);
    m.height = (u8)(random32() % 128);
    m.build = (u8)(random32() % 128);
    new_id(&m);
    return m;
}

/* ---- the user's Miis: miis/<name>.charinfo ------------------------------------------- */
static CharInfo database_miis[DATABASE_MAX];
static u32 database_count;

/* Within the ranges the service accepts (nn::mii's CharInfo verification): a Mii outside
 * them could index past the game's face-part tables. */
static int valid(const CharInfo *m) {
    static const u8 zero[16];
    if (!memcmp(m->create_id, zero, 16) || !m->nickname[0] || m->nickname[10]) return 0;
    const u8 *v = &m->font_region;
    static const u8 max[] = {   /* font_region ... mole_y, in the struct's order */
        3, 11, 1, 127, 127, 1, 3,  11, 9, 11, 11,  131, 99, 1,  59, 99, 7, 6, 7, 12, 18,
        23, 99, 8, 6, 11, 12, 18,  17, 8, 18,  35, 99, 8, 6, 18,  99, 5, 5, 8, 16,
        19, 99, 7, 20,  1, 8, 16, 30,
    };
    _Static_assert(sizeof max == offsetof(CharInfo, padding) - offsetof(CharInfo, font_region), "limits");
    for (u32 i = 0; i < sizeof max; i++)
        if (v[i] > max[i]) return 0;
    return m->eyebrow_y >= 3;   /* (kept 3 higher than in the console's stored form) */
}

static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void load_database(void) {
    static char names[DATABASE_MAX][MAX_PATH];
    u32 n = 0;
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(MIIS_DIR "\\*", &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        const char *dot = strrchr(fd.cFileName, '.');
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && dot && !_stricmp(dot, ".charinfo") && n < DATABASE_MAX)
            snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    qsort(names, n, sizeof *names, by_name);
    for (u32 i = 0; i < n; i++) {
        char path[MAX_PATH + 8];
        snprintf(path, sizeof path, MIIS_DIR "/%s", names[i]);
        FILE *f = fopen(path, "rb");
        CharInfo m;
        size_t got = f ? fread(&m, 1, sizeof m, f) : 0;
        int extra = f && fgetc(f) != EOF;
        if (f) fclose(f);
        if (got != sizeof m || extra || !valid(&m)) {
            fprintf(stderr, "[mii] %s/%s is not a Mii WindowSmash can use: skipped\n", MIIS_DIR, names[i]);
            continue;
        }
        int dup = 0;
        for (u32 k = 0; k < database_count; k++) dup |= !memcmp(database_miis[k].create_id, m.create_id, 16);
        if (dup) {
            fprintf(stderr, "[mii] %s/%s is a Mii already loaded (the same id): skipped\n", MIIS_DIR, names[i]);
            continue;
        }
        database_miis[database_count++] = m;
    }
    if (database_count) fprintf(stderr, "[mii] %u of your Miis, from %s/\n", database_count, MIIS_DIR);
}

static i32 database_index(const u8 *create_id) {
    for (u32 i = 0; i < database_count; i++)
        if (!memcmp(database_miis[i].create_id, create_id, 16)) return (i32)i;
    return -1;
}

static u32 count(u32 source) {
    return (source & SOURCE_DATABASE ? database_count : 0) + (source & SOURCE_DEFAULT ? DEFAULT_COUNT : 0);
}

/* Get: fill the output buffer with CharInfos (or CharInfoElements, which add a u32 source):
 * the user's Miis, then the defaults. */
static void get(Ipc *ipc, u32 element_size) {
    u32 source = ipc_u32(ipc, 0);
    u64 size, b = ipc_out_buffer(ipc, 0, &size);
    u32 n = 0, mine = source & SOURCE_DATABASE ? database_count : 0;
    for (u32 i = 0; i < count(source) && b && (n + 1) * element_size <= size; i++, n++) {
        CharInfo m = i < mine ? database_miis[i] : build_default(i - mine);
        memcpy(P(void, b + (u64)n * element_size), &m, sizeof m);
        if (element_size > sizeof m) st32(b + (u64)n * element_size + sizeof m, i < mine ? ELEMENT_DATABASE : ELEMENT_DEFAULT);
    }
    if (b) ipc_wrote(ipc, 0, (u64)n * element_size);
    IPC_PUSH(ipc, n);
}

static void database(IpcObject *self, Ipc *ipc) {
    CharInfo m;
    switch (ipc->command) {
    case 0: IPC_PUSH(ipc, (u8)0); return;                                  /* IsUpdated */
    case 1: IPC_PUSH(ipc, (u8)0); return;                                  /* IsFullDatabase */
    case 2: IPC_PUSH(ipc, count(ipc_u32(ipc, 0))); return;                 /* GetCount */
    case 3: get(ipc, sizeof(CharInfo) + 4); return;                        /* Get (CharInfoElement) */
    case 4: get(ipc, sizeof(CharInfo)); return;                            /* Get1 (CharInfo) */
    case 5: ipc_push(ipc, ipc->in, sizeof(CharInfo)); return;             /* UpdateLatest: already latest */
    case 6: m = build_random(ipc_u32(ipc, 0), ipc_u32(ipc, 4), ipc_u32(ipc, 8)); ipc_push(ipc, &m, sizeof m); return;
    case 7: m = build_default(ipc_u32(ipc, 0) % DEFAULT_COUNT); ipc_push(ipc, &m, sizeof m); return;
    case 8: case 9: IPC_PUSH(ipc, (u32)0); return;                         /* Get2/Get3 (store data): none */
    case 11: IPC_PUSH(ipc, database_index(ipc->in)); return;               /* FindIndex(create id, special) */
    case 21: IPC_PUSH(ipc, database_index(ipc->in)); return;               /* GetIndex(CharInfo): by its id */
    case 22: return;                                                       /* SetInterfaceVersion */
    case 23: case 24:                                                      /* Convert(Ver3StoreData / CoreData) */
        m = build_default(0); new_id(&m); ipc_push(ipc, &m, sizeof m); return;
    }
    ipc_unknown(ipc, self);
}

static void static_service(IpcObject *self, Ipc *ipc) {
    if (ipc->command == 0) {   /* GetDatabaseService(key code) */
        ipc_object(ipc, ipc_new_object("mii:IDatabaseService", database, NULL));
        return;
    }
    ipc_unknown(ipc, self);
}

/* "Create Mii" (Mii Fighters, and the like): the system's Mii editor, a library applet. There
 * is none (hle_applet.c), so it behaves as if the player opened it and backed out: the game
 * keeps its list as it is. Without this, starting the applet reads a null applet creator. */
HLE(_ZN2nn3mii9AppendMiiEPi) { c->x[0] = RESULT(162, 1); }

IpcObject *service_mii(const char *name) {
    static int loaded;
    if (!strcmp(name, "mii:e") || !strcmp(name, "mii:u")) {
        if (!loaded) { loaded = 1; load_database(); }
        return ipc_new_object(name, static_service, NULL);
    }
    return NULL;
}
