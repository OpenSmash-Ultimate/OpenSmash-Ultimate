/* SSBU 13.0.5: update nu::Material properties through its own setter, which marks
 * shader uniforms dirty. Replacing the serialized MATL buffer alone cannot do this.
 *
 * Verified against generated main C: destructor 3857e00; 38a97d0 constructs a Material (vptr 526a808,
 * libc++ strings 108/120 = material/shader label); property nodes are next+0,
 * descriptor+10 (ID+18), variant+20 (tag5, inline float4+28). 38a9ca0 updates an
 * existing property and marks D8 + 8*(ID/64). 38ac560 consumes those bits and
 * calls 38af680 to update the shader parameters. Never search for naked float4s.
 * Exact build ID, names, old values of edited vectors, bounded structures, and unambiguous
 * watched-file ownership are required before calling the guest setter.
 */
#include "host.h"
#include "mod_material.h"
#include <stdio.h>
#include <stdlib.h>

enum { MAT_MAX = 256, ATTR_MAX = 128, LIVE_MAX = 256, NODE_MAX = 1024 };
typedef struct Blob { const u8 *p; u32 n; } Blob;
typedef struct Attr { u32 id, type; Blob value; } Attr;
typedef struct Entry { const char *label, *shader; Attr attrs[ATTR_MAX]; u32 count; } Entry;
typedef struct File { Blob blob; u32 entries, count; } File;
typedef struct Update { u64 object, node; const u8 *value; } Update;
typedef struct Pending { u64 object, node; u32 id, request; u8 old[16], value[16]; } Pending;
static Pending pending[LIVE_MAX];
static u32 pending_count;
typedef struct Request { u64 id, started; u32 remaining, applied; int failed; } Request;
static Request requests[64];
static u64 next_request = 1;
/* 36a8320 copies each material's animation baseline (loaded with the model)
 * into the live value every frame, which undid a one-shot edit on the next
 * frame. Substitute the edited value only on that path and only while the
 * baseline still equals the value the edit replaced, so real animation keys win. */
enum { OVERRIDE_MAX = 512, RESET_CALLER = 0x36a8574 };
typedef struct Override { u64 object; u32 id; u8 baseline[16], value[16]; } Override;
static Override overrides[OVERRIDE_MAX];
static volatile LONG override_count;

/* Track lifetime rather than scanning freed heap storage or pausing on every page. */
static SRWLOCK material_lock = SRWLOCK_INIT;
static u64 live_materials[16384], live_keys[16384];
static u8 live_rendered[16384];
/* Index chains avoid scanning every material at every render update. Entries
 * store index+1 so zero is the empty chain. No tombstones accumulate across
 * matches; moving the last live entry repairs its incoming link. */
static u32 live_heads[32768], live_next[16384];
static ULONGLONG material_deadline;
static u64 label_key(const char *text, u64 length) {
    u64 hash = 14695981039346656037ull;
    for (u64 i = 0; i < length; i++) hash = (hash ^ (u8)text[i]) * 1099511628211ull;
    return hash;
}
static u64 constructed_label_key(u64 object) {
    /* The constructor just initialized this string; the object is still owned
     * by its caller, so no VM walk or heap search is needed to index it. */
    const u8 *s = (const u8 *)G2H(object) + 0x108;
    u64 length = s[0] >> 1, address = object + 0x109;
    if (s[0] & 1) { memcpy(&length, s + 8, 8); memcpy(&address, s + 16, 8); }
    if (length >= 512 || (!(s[0] & 1) && length > 22)) return 0;
    return label_key(G2H(address), length);
}
static u32 live_count;
static u64 material_render_call_count, render_tracked_calls;
static int material_trace;
static struct { u64 object, node; u32 id, remaining; } trace_followup;
static u32 trace_setter_remaining;
static u32 live_bucket(u64 object) {
    object ^= object >> 33; object *= 0xff51afd7ed558ccdull; object ^= object >> 33;
    return (u32)object & 32767;
}
static u32 *live_link(u64 object) {
    u32 *link = &live_heads[live_bucket(object)];
    while (*link && live_materials[*link - 1] != object) link = &live_next[*link - 1];
    return link;
}
static u32 live_index(u64 object) {
    u32 at = *live_link(object); return at ? at - 1 : ~0u;
}
static int tracking_ready, tracking_overflow;
static GuestFn *material_ctor_original, *material_dtor_original, *material_render_original;
static void material_render(Cpu *c);
typedef struct GpuTrace { u64 object; const u32 *ids; u32 count, current, uniforms, uploads; } GpuTrace;
static _Thread_local GpuTrace *gpu_trace;
static GuestFn *uniform_trace_original, *upload_trace_original;
static GuestFn *setter_original;
static void material_setter(Cpu *c);
static void uniform_trace(Cpu *c), upload_trace(Cpu *c);
/* material_lock is held. Cancel the entire remainder as soon as a member
 * becomes invalid, so a failed transaction cannot keep applying later. */
static void cancel_request(u32 request) {
    requests[request].failed = 1;
    requests[request].remaining = 0;
    for (u32 i = 0; i < pending_count;) {
        if (pending[i].request == request) pending[i] = pending[--pending_count]; else i++;
    }
}
int mod_material_result(u64 id, int acknowledge, char *reason, u32 size) {
    int result = MOD_MATERIAL_FAILED;
    AcquireSRWLockExclusive(&material_lock);
    snprintf(reason, size, "material request is unavailable");
    for (u32 i = 0; i < sizeof requests / sizeof requests[0]; i++) if (requests[i].id == id && id) {
        Request *r = &requests[i];
        if (r->remaining && GetTickCount64() - r->started > 5000) cancel_request(i);
        result = r->remaining ? MOD_MATERIAL_PENDING : r->failed ?
            (r->applied ? MOD_MATERIAL_PARTIAL : MOD_MATERIAL_FAILED) : MOD_MATERIAL_APPLIED;
        snprintf(reason, size, "%s (%u vector%s applied)",
                 result == MOD_MATERIAL_PENDING ? "waiting for material render update" :
                 result == MOD_MATERIAL_APPLIED ? "material render update completed" :
                 result == MOD_MATERIAL_PARTIAL ? "material update partially applied; restart the game before retrying" :
                 "material update canceled before applying; will retry", r->applied, r->applied == 1 ? "" : "s");
        if (acknowledge && result != MOD_MATERIAL_PENDING) memset(r, 0, sizeof *r);
        break;
    }
    ReleaseSRWLockExclusive(&material_lock);
    return result;
}
static void remember_material(u64 object) {
    AcquireSRWLockExclusive(&material_lock);
    if (live_index(object) != ~0u) { ReleaseSRWLockExclusive(&material_lock); return; }
    if (live_count < sizeof live_materials / sizeof live_materials[0]) {
        live_keys[live_count] = constructed_label_key(object); live_rendered[live_count] = 0;
        u32 bucket = live_bucket(object); live_next[live_count] = live_heads[bucket]; live_heads[bucket] = live_count + 1;
        live_materials[live_count++] = object;
    }
    else tracking_overflow = 1;
    ReleaseSRWLockExclusive(&material_lock);
}
static void forget_material(u64 object) {
    AcquireSRWLockExclusive(&material_lock);
    if (trace_followup.object == object) trace_followup.remaining = 0;
    u32 *link = live_link(object);
    if (*link) {
        u32 i = *link - 1, last = live_count - 1;
        *link = live_next[i];
        if (i != last) {
            u32 *moved = live_link(live_materials[last]);
            *moved = i + 1;
            live_materials[i] = live_materials[last]; live_keys[i] = live_keys[last];
            live_rendered[i] = live_rendered[last]; live_next[i] = live_next[last];
        }
        live_count--;
    }
    for (u32 i = 0; i < pending_count;) {
        if (pending[i].object == object) { cancel_request(pending[i].request); i = 0; } else i++;
    }
    for (LONG i = 0; i < override_count;) {
        if (overrides[i].object == object) overrides[i] = overrides[--override_count]; else i++;
    }
    ReleaseSRWLockExclusive(&material_lock);
}
static void material_construct(Cpu *c) {
    u64 object = c->x[0]; material_ctor_original(c); remember_material(object);
}
static void material_destruct(Cpu *c) {
    forget_material(c->x[0]); material_dtor_original(c);
}
void mod_material_init(void) {
    /* host.c calls this after modules are mapped, BEFORE any guest thread starts. */
    material_trace = getenv("SSBU_MOD_REFRESH_TRACE") != NULL;
    if (!rt_module_main.build_id || strcmp(rt_module_main.build_id, "21450C647B8C5940ED5B68218EA128FD3C5D597D")) return;
    u64 destructor = rt_hook_function(rt_module_main.base + 0x3857e00, rt_native(material_destruct, "material lifetime end"));
    if (!destructor) return;
    material_dtor_original = rt_lookup(destructor);
    u64 constructor = rt_hook_function(rt_module_main.base + 0x38a97d0, rt_native(material_construct, "material lifetime start"));
    if (!constructor) return;
    material_ctor_original = rt_lookup(constructor);
    u64 render = rt_hook_function(rt_module_main.base + 0x38ac560, rt_native(material_render, "material render update"));
    if (!render) return;
    material_render_original = rt_lookup(render);
    u64 setter = rt_hook_function(rt_module_main.base + 0x38a9ca0, rt_native(material_setter, "material baseline reset"));
    if (!setter) return;
    setter_original = rt_lookup(setter);
    tracking_ready = material_ctor_original && material_dtor_original && material_render_original && setter_original;
    if (material_trace) {
        u64 uniform = rt_hook_function(rt_module_main.base + 0x38af680, rt_native(uniform_trace, "trace mod uniform propagation"));
        u64 upload = rt_hook_function(rt_module_main.base + 0x38b0440, rt_native(upload_trace, "trace mod UBO upload"));
        uniform_trace_original = uniform ? rt_lookup(uniform) : NULL;
        upload_trace_original = upload ? rt_lookup(upload) : NULL;
        fprintf(stderr, "[mods] GPU propagation trace uniform=%d upload=%d\n", uniform_trace_original != NULL, upload_trace_original != NULL);
    }

}


static u64 mm_u64(const void *p) { u64 n; memcpy(&n, p, 8); return n; }
static u32 mm_u32(const void *p) { u32 n; memcpy(&n, p, 4); return n; }
static int within(Blob b, u64 p, u64 n) { return p <= b.n && n <= b.n - p; }
static int relative(Blob b, u32 field, u32 bytes, u32 *out) {
    if (!within(b, field, 8)) return 0;
    u64 off = mm_u64(b.p + field);
    if (!off || off > b.n - field || !within(b, field + off, bytes)) return 0;
    *out = field + (u32)off; return 1;
}
static const char *text(Blob b, u32 field) {
    u32 at;
    if (!relative(b, field, 1, &at) || !memchr(b.p + at, 0, b.n - at)) return NULL;
    return (const char *)b.p + at;
}
static int file_read(const u8 *data, u32 size, File *out) {
    Blob b = {data, size};
    if (size < 40 || memcmp(data, "HBSS", 4) || memcmp(data + 16, "LTAM\1\0\6\0", 8)) return 0;
    u64 count = mm_u64(data + 32); u32 at;
    if (!count || count > MAT_MAX || !relative(b, 24, (u32)count * 32, &at)) return 0;
    *out = (File){b, at, (u32)count}; return 1;
}
static int entry_read(File f, u32 index, Entry *e) {
    u32 at = f.entries + index * 32, attrs;
    if (index >= f.count) return 0;
    e->label = text(f.blob, at); e->shader = text(f.blob, at + 24);
    u64 count = mm_u64(f.blob.p + at + 16);
    if (!e->label || !e->shader || count > ATTR_MAX || !count ||
        !relative(f.blob, at + 8, (u32)count * 24, &attrs)) return 0;
    e->count = (u32)count;
    for (u32 i = 0; i < e->count; i++) {
        u32 a = attrs + i * 24, value;
        u64 id = mm_u64(f.blob.p + a), type = mm_u64(f.blob.p + a + 16);
        u32 bytes = type == 1 || type == 2 ? 4 : type == 5 ? 16 : type == 11 ? 8 :
                    type == 14 ? 56 : type == 16 ? 20 : type == 17 ? 48 : type == 18 ? 32 : 0;
        if (id >= 0x16e || !bytes || !relative(f.blob, a + 8, bytes, &value)) return 0;
        for (u32 j = 0; j < i; j++) if (e->attrs[j].id == id) return 0;
        Blob v = {f.blob.p + value, bytes};
        if (type == 11) {
            const char *s = text(f.blob, value); if (!s) return 0;
            v = (Blob){(const u8 *)s, (u32)strlen(s) + 1};
        }
        e->attrs[i] = (Attr){(u32)id, (u32)type, v};
    }
    return 1;
}
static const Attr *attribute(const Entry *e, u32 id) {
    for (u32 i = 0; i < e->count; i++) if (e->attrs[i].id == id) return &e->attrs[i];
    return NULL;
}
static int same(Blob a, Blob b) { return a.n == b.n && !memcmp(a.p, b.p, a.n); }
static int names(const Entry *a, const Entry *b) { return !strcmp(a->label, b->label) && !strcmp(a->shader, b->shader); }
/* 13.0.5 main+35f1fa0 parses SFX_PBS_'s 16-digit number, then calls
 * 35f37b0 with x3=0x0020000000000000 and variant index1. That helper adds
 * x3 to the number, formats it as 16 hex digits and appends the unchanged
 * suffix. Rendering may use that derived normal shader rather than MATL's
 * original name. Accept only this verified flag; shadow/depth names and all
 * other differences remain distinct. */
static int shader_normal_variant(const char *name, char *out, u32 size) {
    if (strncmp(name, "SFX_PBS_", 8) || strlen(name) < 26 || name[24] != '_') return 0;
    u64 number = 0;
    for (u32 i = 8; i < 24; i++) {
        unsigned char ch = (unsigned char)name[i]; u32 digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else return 0;
        number = (number << 4) | digit;
    }
    if (number & 0x0020000000000000ull) return 0;
    int n = snprintf(out, size, "SFX_PBS_%016llx%s", (unsigned long long)(number + 0x0020000000000000ull), name + 24);
    return n > 0 && (u32)n < size;
}
static int shader_identity(const char *a, const char *b) {
    char variant[512];
    return !strcmp(a, b) || (shader_normal_variant(a, variant, sizeof variant) && !strcmp(variant, b)) ||
           (shader_normal_variant(b, variant, sizeof variant) && !strcmp(variant, a));
}
static int changed_vectors_same(const Entry *old, const Entry *next, const Entry *other) {
    for (u32 i = 0; i < old->count; i++) if (old->attrs[i].type == 5) {
        const Attr *n = attribute(next, old->attrs[i].id);
        if (same(old->attrs[i].value, n->value)) continue;
        const Attr *o = attribute(other, old->attrs[i].id);
        if (!o || o->type != 5 || !same(old->attrs[i].value, o->value)) return 0;
    }
    return 1;
}
static int readable(u64 address, u32 bytes) {
    if (material_deadline && GetTickCount64() > material_deadline) return 0;
    if (address < 0x10000 || address >= 0x8000000000ull || bytes > 0x8000000000ull - address) return 0;
    u8 *p = G2H(address), *end = p + bytes;
    while (p < end) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
        u8 *next = (u8 *)m.BaseAddress + m.RegionSize;
        if (next <= p) return 0;
        p = next;
    }
    return 1;
}
static int guest_string(u64 at, const char *expected) {
    if (!readable(at, 24)) return 0;
    const u8 *s = G2H(at); u64 n = s[0] >> 1, p = at + 1;
    if (s[0] & 1) { n = mm_u64(s + 8); p = mm_u64(s + 16); }
    if (!(s[0] & 1) && n > 22) return 0;
    return n == strlen(expected) && n < 512 && readable(p, (u32)n + 1) && !memcmp(G2H(p), expected, (size_t)n + 1);
}
static int guest_shader(u64 at, const char *expected) {
    char variant[512];
    return guest_string(at, expected) ||
           (shader_normal_variant(expected, variant, sizeof variant) && guest_string(at, variant));
}
static u64 node_for(u64 object, u32 id) {
    u64 node = mm_u64((const u8 *)G2H(object) + 0x20);
    for (u32 i = 0; node && i < NODE_MAX; i++) {
        if (!readable(node, 0x38)) return 0;
        const u8 *p = G2H(node);
        if (mm_u32(p + 0x18) == id) return node;
        u64 next = mm_u64(p); if (next == node) return 0;
        node = next;
    }
    return 0;
}
static u64 effective_variant(u64 object, u64 node, u32 id) {
    /* 38a9ca0's x3=1 path uses these cached variant pointers. They may point
     * away from the generic property node. In particular CustomVector8 (160)
     * uses this+48: changing only the node leaves its live color unchanged. */
    u32 field = id == 152 ? 0x38 : id == 155 ? 0x40 : id == 160 ? 0x48 :
                id == 165 ? 0x50 : id == 166 ? 0x58 : id == 324 ? 0x60 : id == 326 ? 0x68 : 0;
    u64 variant = node + 0x20;
    if (field) {
        if (!readable(object + field, 8)) return 0;
        u64 cached = mm_u64((u8 *)G2H(object) + field);
        if (cached) variant = cached;
    }
    return readable(variant, 24) && mm_u32(G2H(variant)) == 5 ? variant : 0;
}
static int matches(u64 object, const Entry *old, const Entry *next) {
    if (!readable(object, 0x138) || !guest_string(object + 0x108, old->label) ||
        !guest_shader(object + 0x120, old->shader)) return 0;
    int vectors = 0;
    for (u32 i = 0; i < old->count; i++) if (old->attrs[i].type == 5) {
        const Attr *n = attribute(next, old->attrs[i].id);
        if (same(old->attrs[i].value, n->value)) continue;
        u64 node = node_for(object, old->attrs[i].id);
        if (!node || mm_u32((const u8 *)G2H(node) + 0x20) != 5) return 0;
        u64 variant = effective_variant(object, node, old->attrs[i].id);
        if (!variant) return 0;
        const void *value = (const u8 *)G2H(variant) + 8;
        if (memcmp(value, old->attrs[i].value.p, 16)) return 0;
        vectors++;
    }
    return vectors != 0;
}

static void trace_material_value(const char *phase, u64 object, u64 node, u32 id) {
    if (!readable(node + 0x28, 16) || !readable(object + 0xd8 + (id / 64) * 8, 8)) return;
    float v[4]; memcpy(v, (u8 *)G2H(node) + 0x28, 16);
    u64 dirty = mm_u64((u8 *)G2H(object) + 0xd8 + (id / 64) * 8);
    fprintf(stderr, "[mods] propagation %s object=%llx id=%u vector=(%g,%g,%g,%g) dirty=%llx bit=%d\n", phase,
            (unsigned long long)object, id, v[0], v[1], v[2], v[3], (unsigned long long)dirty,
            (dirty & (1ull << (id & 63))) != 0);
    u64 effective = effective_variant(object, node, id);
    if (effective) {
        memcpy(v, (u8 *)G2H(effective) + 8, 16);
        fprintf(stderr, "[mods] propagation %s effective_variant=%llx vector=(%g,%g,%g,%g)\n", phase,
                (unsigned long long)effective, v[0], v[1], v[2], v[3]);
    }
}
/* Caller holds material_lock exclusively. */
static void remember_override(u64 object, u32 id, const u8 *baseline, const u8 *value) {
    LONG i = 0;
    while (i < override_count && (overrides[i].object != object || overrides[i].id != id)) i++;
    if (i == override_count) {
        if (i == OVERRIDE_MAX) return; /* capacity is reserved when queueing */
        overrides[i] = (Override){.object = object, .id = id};
        memcpy(overrides[i].baseline, baseline, 16);
        override_count++;
    }
    memcpy(overrides[i].value, value, 16);
    if (!memcmp(overrides[i].baseline, value, 16)) overrides[i] = overrides[--override_count];
}
static void apply_override(Cpu *c) {
    /* Only the baseline reset caller: material_render's own rt_call holds the
     * lock exclusively and never returns to this address. */
    AcquireSRWLockShared(&material_lock);
    if (readable(c->x[1] + 8, 4) && readable(c->x[2], 24) && mm_u32(G2H(c->x[2])) == 5) {
        u32 id = mm_u32((u8 *)G2H(c->x[1]) + 8);
        u8 *value = (u8 *)G2H(c->x[2]) + 8;
        for (LONG i = 0; i < override_count; i++) {
            if (overrides[i].object == c->x[0] && overrides[i].id == id) {
                if (!memcmp(value, overrides[i].baseline, 16)) memcpy(value, overrides[i].value, 16);
                break;
            }
        }
    }
    ReleaseSRWLockShared(&material_lock);
}
static void material_setter(Cpu *c) {
    if (ReadAcquire(&override_count) && c->x[30] == rt_module_main.base + RESET_CALLER) apply_override(c);
    if (material_trace && trace_setter_remaining && c->x[0] == trace_followup.object &&
        readable(c->x[1], 12) && mm_u32((u8 *)G2H(c->x[1]) + 8) == trace_followup.id) {
        trace_setter_remaining--;
        float value[4] = {0};
        u32 tag = readable(c->x[2], 24) ? mm_u32(G2H(c->x[2])) : ~0u;
        if (tag == 5) memcpy(value, (u8 *)G2H(c->x[2]) + 8, 16);
        fprintf(stderr, "[mods] reset source material=%llx id=%u caller=%llx variant=%llx tag=%u value=(%g,%g,%g,%g) mode=%llu r19=%llx r20=%llx r21=%llx\n",
                (unsigned long long)c->x[0], trace_followup.id, (unsigned long long)c->x[30],
                (unsigned long long)c->x[2], tag, value[0], value[1], value[2], value[3],
                (unsigned long long)c->x[3], (unsigned long long)c->x[19],
                (unsigned long long)c->x[20], (unsigned long long)c->x[21]);
    }
    setter_original(c);
}
static int trace_string(u64 address, char *out, u32 capacity) {
    if (!readable(address, 24)) return 0;
    const u8 *s = G2H(address); u64 n = s[0] >> 1, at = address + 1;
    if (s[0] & 1) { n = mm_u64(s + 8); at = mm_u64(s + 16); }
    if (n >= capacity || !readable(at, (u32)n + 1)) return 0;
    memcpy(out, G2H(at), (size_t)n); out[n] = 0; return 1;
}
/* Bounded, read-only equivalent of 38af000's metadata lookup, followed by
 * 38b0440's (stage, buffer) binding. Only runs once for a pending TRACE update. */
static void trace_uniform_buffers(u64 pack, u32 id) {
    if (!readable(pack, 0x28)) return;
    char name[128];
    if (!trace_string(mm_u64((u8 *)G2H(pack) + 8), name, sizeof name)) return;
    u64 descriptor = mm_u64((u8 *)G2H(pack) + 0x10), map = mm_u64((u8 *)G2H(pack) + 0x20);
    if (!readable(descriptor, 16) || !readable(map, 0x20)) return;
    u64 stages = mm_u64((u8 *)G2H(descriptor) + 8); u32 slots = 0, mapped = 0;
    for (u32 stage = 0; stage < 6; stage++) {
        u64 slot = stages + stage * 32;
        if (!readable(slot + 0x48, 8)) continue;
        u64 shader = mm_u64((u8 *)G2H(slot) + 0x48);
        if (!shader || !readable(shader + 0xa8, 0x20)) continue;
        u64 entry = mm_u64((u8 *)G2H(shader) + 0xb8);
        for (u32 n = 0; entry && n < 512; n++) {
            if (!readable(entry, 0x50)) break;
            if (guest_string(entry + 0x10, name)) {
                u32 buffer_id = mm_u32((u8 *)G2H(entry) + 0x44), offset = mm_u32((u8 *)G2H(entry) + 0x48);
                slots++;
                u64 node = mm_u64((u8 *)G2H(map) + 0x10);
                for (u32 b = 0; node && b < 512; b++) {
                    if (!readable(node, 0x28)) break;
                    if (mm_u32((u8 *)G2H(node) + 0x10) == stage && mm_u32((u8 *)G2H(node) + 0x14) == buffer_id) {
                        u64 buffer = mm_u64((u8 *)G2H(node) + 0x20);
                        if (!readable(buffer, 0x60)) break;
                        u64 base = mm_u64((u8 *)G2H(buffer) + 0x58);
                        if (!base || offset > 0x100000 || !readable(base + offset, 16)) break;
                        float v[4]; memcpy(v, G2H(base + offset), 16); mapped++;
                        fprintf(stderr, "[mods] UBO trace id=%u name=%s stage=%u buffer=%u base=%llx offset=%u address=%llx vector=(%g,%g,%g,%g)\n",
                                id, name, stage, buffer_id, (unsigned long long)base, offset,
                                (unsigned long long)(base + offset), v[0], v[1], v[2], v[3]);
                        break;
                    }
                    node = mm_u64(G2H(node));
                }
                break;
            }
            entry = mm_u64(G2H(entry));
        }
    }
    fprintf(stderr, "[mods] UBO trace id=%u name=%s slots=%u mapped=%u\n", id, name, slots, mapped);
}
static void uniform_trace(Cpu *c) {
    GpuTrace *t = gpu_trace; u32 previous = t ? t->current : 0;
    if (t) {
        t->current = 0;
        if (readable(c->x[2], 12)) {
            u32 id = mm_u32((u8 *)G2H(c->x[2]) + 8);
            for (u32 i = 0; i < t->count; i++) if (t->ids[i] == id) {
                t->current = id + 1; t->uniforms++;
                fprintf(stderr, "[mods] UBO trace material=%llx id=%u target=%llx variant=%llx\n",
                        (unsigned long long)t->object, id, (unsigned long long)c->x[0], (unsigned long long)c->x[3]);
                break;
            }
        }
    }
    uniform_trace_original(c);
    if (t) t->current = previous;
}
static void upload_trace(Cpu *c) {
    GpuTrace *t = gpu_trace; u64 pack = c->x[0]; u32 id = t && t->current ? t->current - 1 : ~0u;
    if (id != ~0u) {
        t->uploads++;
        fprintf(stderr, "[mods] UBO trace upload id=%u pack=%llx type=%u size=%u\n", id, (unsigned long long)pack, (u32)c->x[2], (u32)c->x[3]);
        if (c->x[3] == 16 && readable(c->x[1], 16)) {
            float v[4]; memcpy(v, G2H(c->x[1]), 16);
            fprintf(stderr, "[mods] UBO trace source=(%g,%g,%g,%g)\n", v[0], v[1], v[2], v[3]);
        }
    }
    upload_trace_original(c);
    if (id != ~0u) trace_uniform_buffers(pack, id);
}
/* Execute changes on the material's normal render-update thread, immediately
 * before dirty parameters are consumed by 38ac560/38af680. A fighter-frame
 * setter call can race the renderer or be overwritten by the training reset.
 * Never replay the model builder from a fighter callback. */
static void material_render(Cpu *c) {
    u64 object = c->x[0];
    AcquireSRWLockExclusive(&material_lock);
    u32 index = live_index(object);
    int live = index != ~0u, first_render = live && !live_rendered[index];
    material_render_call_count++;
    if (first_render) {
        /* A constructor's caller may finish naming a material after returning
         * from the base constructor. Capture the identity used by rendering. */
        live_keys[index] = constructed_label_key(object); live_rendered[index] = 1;
    }
    if (live) render_tracked_calls++;
    u64 followup_node = 0; u32 followup_id = 0;
    if (material_trace && trace_followup.object == object && trace_followup.remaining) {
        if (live) {
            followup_node = trace_followup.node; followup_id = trace_followup.id;
            trace_followup.remaining--;
        } else trace_followup.remaining = 0;
    }
    static u32 initial_logs, goku_logs, untracked_logs;
    if (material_trace && (initial_logs < 5 || (first_render && goku_logs < 10) || (!live && untracked_logs < 5))) {
        char label[128] = {0};
        if (readable(object + 0x108, 24)) {
            const u8 *s = G2H(object + 0x108); u64 n = s[0] >> 1, at = object + 0x109;
            if (s[0] & 1) { n = mm_u64(s + 8); at = mm_u64(s + 16); }
            if (n < sizeof label && readable(at, (u32)n)) memcpy(label, G2H(at), (size_t)n);
        }
        int report = initial_logs++ < 5;
        if (strstr(label, "goku") && goku_logs < 10) { goku_logs++; report = 1; }
        if (!live && untracked_logs < 5) { untracked_logs++; report = 1; }
        if (report)
            fprintf(stderr, "[mods] render trace object=%llx tracked=%d vptr=%llx label=%s\n",
                    (unsigned long long)object, live,
                    (unsigned long long)(readable(object, 8) ? mm_u64(G2H(object)) : 0), label);
    }
    if (!live || !pending_count) {
        ReleaseSRWLockExclusive(&material_lock);
        GpuTrace *previous = gpu_trace; gpu_trace = NULL;
        material_render_original(c); gpu_trace = previous;
        if (followup_node) trace_material_value("followup render", object, followup_node, followup_id);
        return;
    }
    Cpu saved = *c;
    u64 scratch = c->sp >= 0x100 ? c->sp - 0x100 : 0;
    u8 scratch_was[0x100];
    int have_scratch = live && pending_count && readable(scratch, sizeof scratch_was);
    u64 trace_nodes[LIVE_MAX]; u32 trace_ids[LIVE_MAX], trace_count = 0;
    if (have_scratch) memcpy(scratch_was, G2H(scratch), sizeof scratch_was);
    for (u32 i = 0; have_scratch && i < pending_count;) {
        Pending *p = &pending[i];
        if (p->object != object) { i++; continue; }
        /* Resolve the node again: gameplay may have replaced its property list
         * after the request was queued. Never dereference a saved freed node. */
        u64 node = node_for(object, p->id);
        u64 effective = node ? effective_variant(object, node, p->id) : 0;
        if (!node || node != p->node || mm_u32((u8 *)G2H(node) + 0x20) != 5 ||
            !effective || memcmp((u8 *)G2H(effective) + 8, p->old, 16)) {
            fprintf(stderr, "[mods] queued material vector invalidated before render; reload the fighter\n");
            cancel_request(p->request); i = 0; continue;
        }
        u8 variant[24] = {5}; memcpy(variant + 8, p->value, 16);
        memcpy(G2H(scratch), variant, sizeof variant);
        *c = saved; c->sp = scratch;
        c->x[0] = object; c->x[1] = node + 0x10; c->x[2] = scratch; c->x[3] = 1;
        rt_call(c, rt_module_main.base + 0x38a9ca0);
        if (material_trace && trace_count < LIVE_MAX) {
            trace_nodes[trace_count] = node; trace_ids[trace_count++] = p->id;
            trace_material_value("after setter", object, node, p->id);
        }
        effective = effective_variant(object, node, p->id);
        if (!effective || memcmp((u8 *)G2H(effective) + 8, p->value, 16)) {
            fprintf(stderr, "[mods] material setter did not update the live vector; request canceled\n");
            cancel_request(p->request); i = 0; continue;
        }
        remember_override(object, p->id, p->old, p->value);
        requests[p->request].applied++;
        requests[p->request].remaining--;
        if (material_trace) {
            trace_followup.object = object; trace_followup.node = node;
            trace_followup.id = p->id; trace_followup.remaining = 5;
            trace_setter_remaining = 10;
        }
        fprintf(stderr, "[mods] applied queued material vector %u at render update\n", p->id);
        pending[i] = pending[--pending_count];
    }
    if (have_scratch) memcpy(G2H(scratch), scratch_was, sizeof scratch_was);
    *c = saved;
    ReleaseSRWLockExclusive(&material_lock);
    GpuTrace trace = {.object = object, .ids = trace_ids, .count = trace_count}, *previous = gpu_trace;
    gpu_trace = trace_count ? &trace : NULL;
    material_render_original(c);
    gpu_trace = previous;
    if (followup_node) trace_material_value("followup render", object, followup_node, followup_id);
    for (u32 i = 0; i < trace_count; i++) trace_material_value("after render", object, trace_nodes[i], trace_ids[i]);
    if (trace_count) fprintf(stderr, "[mods] UBO trace render complete material=%llx uniforms=%u uploads=%u\n",
                             (unsigned long long)object, trace.uniforms, trace.uploads);
}

int mod_material_refresh(Cpu *c, const ModMaterialFile *files, u32 count, u32 target,
                         const u8 *data, u32 size, u32 *updated, u64 *request, char *reason, u32 reason_size) {
#define FAIL(message) do { snprintf(reason, reason_size, "%s", message); if (locked) { material_deadline = 0; ReleaseSRWLockExclusive(&material_lock); } free(plan); return 0; } while (0)
    Update *plan = NULL; u32 used = 0, inactive = 0; int locked = 0;
    *updated = 0;
    *request = 0;
    if (!rt_module_main.build_id || strcmp(rt_module_main.build_id, "21450C647B8C5940ED5B68218EA128FD3C5D597D"))
        FAIL("this game version has no verified live material layout");
    File old_file, new_file;
    if (target >= count || !file_read(files[target].data, files[target].size, &old_file) ||
        !file_read(data, size, &new_file) || old_file.count != new_file.count) FAIL("material structure changed; reload the fighter");
    plan = calloc(LIVE_MAX, sizeof *plan);
    if (!plan) FAIL("not enough memory for material refresh");
    AcquireSRWLockExclusive(&material_lock); locked = 1;
    material_deadline = GetTickCount64() + 200;
    if (!tracking_ready || tracking_overflow) FAIL("live material tracking is unavailable; restart the game");
    /* Plan the whole operation first: a mismatch must not leave half the file applied. */
    for (u32 e = 0; e < old_file.count; e++) {
        Entry old, next;
        if (!entry_read(old_file, e, &old) || !entry_read(new_file, e, &next) || !names(&old, &next) || old.count != next.count)
            FAIL("material names, shader or structure changed; reload the fighter");
        int changed = 0;
        for (u32 a = 0; a < old.count; a++) {
            const Attr *n = attribute(&next, old.attrs[a].id);
            if (!n || n->type != old.attrs[a].type) FAIL("material parameter structure changed; reload the fighter");
            if (!same(old.attrs[a].value, n->value)) {
                if (n->type != 5) FAIL("live material refresh currently requires vector-only edits; reload the fighter");
                changed++;
            }
        }
        if (!changed) continue;
        /* Two watched assets with the same material identity and old vectors cannot
         * safely be distinguished. Multiple instances of ONE asset are intentional. */
        for (u32 f = 0; f < count; f++) if (f != target) {
            File other;
            if (!file_read(files[f].data, files[f].size, &other)) continue;
            for (u32 j = 0; j < other.count; j++) {
                Entry candidate;
                if (entry_read(other, j, &candidate) && !strcmp(old.label, candidate.label) &&
                    shader_identity(old.shader, candidate.shader) && changed_vectors_same(&old, &next, &candidate))
                    FAIL("another watched asset has the same material identity; reload the fighter");
            }
        }
        int found = 0, identity_found = 0; u32 keyed = 0, rendered = 0, identities = 0;
        u64 key = label_key(old.label, strlen(old.label));
        for (u32 m = 0; m < live_count; m++) {
            if (GetTickCount64() > material_deadline) FAIL("material validation exceeded 200 ms; reload deferred without applying changes");
            if (live_keys[m] != key) continue;
            keyed++;
            if (!live_rendered[m]) continue;
            rendered++;
            u64 object = live_materials[m];
            if (!readable(object, 0x138) || mm_u64(G2H(object)) != rt_module_main.base + 0x526a808 ||
                !guest_string(object + 0x108, old.label) || !guest_shader(object + 0x120, old.shader)) continue;
            identity_found = 1;
            identities++;
            if (!matches(object, &old, &next)) continue;
            found++;
            for (u32 a = 0; a < old.count; a++) {
                const Attr *n = attribute(&next, old.attrs[a].id);
                if (n->type != 5 || same(old.attrs[a].value, n->value)) continue;
                if (used == LIVE_MAX) FAIL("too many live material parameters; reload the fighter");
                plan[used++] = (Update){object, node_for(object, n->id), n->value.p};
            }
        }
        if (!found) {
            if (material_trace) {
                fprintf(stderr, "[mods] material trace %s: tracked=%u same_key=%u rendered=%u identity=%u old_vectors=%d render_calls=%llu render_tracked=%llu\n",
                        old.label, live_count, keyed, rendered, identities, found,
                        (unsigned long long)material_render_call_count, (unsigned long long)render_tracked_calls);
                u32 shown = 0;
                for (u32 m = 0; m < live_count && shown < 3; m++) if (live_keys[m] == key) {
                    u64 object = live_materials[m]; shown++;
                    fprintf(stderr, "[mods] material trace object=%llx rendered=%u vptr=%llx",
                            (unsigned long long)object, live_rendered[m],
                            (unsigned long long)(readable(object, 8) ? mm_u64(G2H(object)) : 0));
                    for (u32 a = 0; a < old.count; a++) {
                        const Attr *n = attribute(&next, old.attrs[a].id);
                        if (n->type != 5 || same(old.attrs[a].value, n->value)) continue;
                        u64 node = readable(object, 0x138) ? node_for(object, n->id) : 0;
                        if (node && readable(node + 0x28, 16)) {
                            float v[4]; memcpy(v, (u8 *)G2H(node) + 0x28, 16);
                            fprintf(stderr, " id=%u node=%llx vector=(%g,%g,%g,%g)", n->id,
                                    (unsigned long long)node, v[0], v[1], v[2], v[3]);
                        }
                        break;
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (identity_found) FAIL("rendered material values differ from the old file; reload the fighter");
            /* One MATL file can contain unused materials (alternate meshes,
             * transformations, etc.). They must not block its active entries;
             * the next normal load reads their replacement from disk. */
            inactive++;
        }
    }
    if (GetTickCount64() > material_deadline) FAIL("material validation exceeded 200 ms; reload deferred without applying changes");
    if (!used) FAIL("no matching rendered material found; select this fighter/costume and try again");
    /* A pending request owns its values; the file reader frees/replaces its
     * snapshots after returning. No guest code is called from this thread. */
    if (used > LIVE_MAX - pending_count) FAIL("material render queue is full; try again after a frame");
    if (used > OVERRIDE_MAX - (u32)override_count) FAIL("too many edited material values this session; restart the game");
    for (u32 i = 0; i < used; i++) for (u32 j = 0; j < pending_count; j++)
        if (pending[j].object == plan[i].object && pending[j].node == plan[i].node)
            FAIL("this material still has a pending render update; try again after a frame");
    u32 slot = 0;
    while (slot < sizeof requests / sizeof requests[0] && requests[slot].id) slot++;
    if (slot == sizeof requests / sizeof requests[0]) FAIL("material request queue is full; try again after a frame");
    u64 id = next_request++; if (!id) id = next_request++;
    requests[slot] = (Request){.id = id, .started = GetTickCount64(), .remaining = used};
    for (u32 i = 0; i < used; i++) {
        Pending *p = &pending[pending_count++];
        p->object = plan[i].object; p->node = plan[i].node;
        p->id = mm_u32((u8 *)G2H(p->node) + 0x18);
        p->request = slot;
        memcpy(p->old, (u8 *)G2H(effective_variant(p->object, p->node, p->id)) + 8, 16);
        memcpy(p->value, plan[i].value, 16);
    }
    (void)c;
    *updated = used;
    *request = id;
    snprintf(reason, reason_size, "queued %u live material vector parameter%s for render update; %u inactive material%s load next use",
             used, used == 1 ? "" : "s", inactive, inactive == 1 ? "" : "s");
    material_deadline = 0; ReleaseSRWLockExclusive(&material_lock); free(plan); return 1;
#undef FAIL
}
