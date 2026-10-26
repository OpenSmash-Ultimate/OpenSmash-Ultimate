/* The server side of the console's IPC, natively.
 *
 * A client writes a message into a buffer (its thread-local region, or a buffer it names)
 * and makes a system call; the kernel hands the message here, the message is answered in
 * place, and the call returns. Message format:
 *
 *   HIPC (transport): two header words; optional special header with a process id and copied
 *   or moved handles; static (X), send (A), receive (B) and exchange (W) buffer descriptors;
 *   raw data; receive-list (C) descriptors after the raw data.
 *   CMIF (commands), in the raw data at the first 16-byte boundary: "SFCI", version, command,
 *   token, arguments. Responses carry "SFCO", version, result, token, values.
 *   Domains: a session can become a domain multiplexing several objects; domain requests
 *   carry a small header (object id, input object ids) in front of the CMIF header, and new
 *   objects come back as ids instead of new sessions.
 *   Control requests (types 5 and 7) manage the session itself. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>

#include "ipc.h"

#define SFCI 0x49434653u
#define SFCO 0x4F434653u

typedef struct Session { int domain; IpcObject *object; IpcObject *ids[256]; } Session;

static SRWLOCK lock = SRWLOCK_INIT;

IpcObject *ipc_new_object(const char *name, void (*handle)(IpcObject *, Ipc *), void *state) {
    IpcObject *o = calloc(1, sizeof *o);
    o->name = name;
    o->handle = handle;
    o->state = state;
    return o;
}

/* ---- argument access ------------------------------------------------------------- */
u32 ipc_u32(Ipc *ipc, u32 offset) { u32 v = 0; if (offset + 4 <= ipc->in_size) memcpy(&v, ipc->in + offset, 4); return v; }
u64 ipc_u64(Ipc *ipc, u32 offset) { u64 v = 0; if (offset + 8 <= ipc->in_size) memcpy(&v, ipc->in + offset, 8); return v; }

void ipc_push(Ipc *ipc, const void *data, u32 size) {
    if (ipc->out_size + size > sizeof ipc->out) rt_fatal("ipc: response too large");
    memcpy(ipc->out + ipc->out_size, data, size);
    ipc->out_size += size;
}

void ipc_copy_handle(Ipc *ipc, u32 handle) { kernel_copy_handle(handle); ipc->copy_out[ipc->ncopy_out++] = handle; }
void ipc_object(Ipc *ipc, IpcObject *object) { ipc->objects_out[ipc->nobjects_out++] = object; }

u64 ipc_out_buffer(Ipc *ipc, u32 i, u64 *size) {
    if (i < ipc->nb && ipc->b[i].addr) { *size = ipc->b[i].size; return ipc->b[i].addr; }
    if (i < ipc->nc) { *size = ipc->c[i].size; return ipc->c[i].addr; }
    *size = 0;
    return 0;
}

u64 ipc_in_buffer(Ipc *ipc, u32 i, u64 *size) {
    if (i < ipc->na && ipc->a[i].addr) { *size = ipc->a[i].size; return ipc->a[i].addr; }
    if (i < ipc->nx) { *size = ipc->x[i].size; return ipc->x[i].addr; }
    *size = 0;
    return 0;
}

void ipc_wrote(Ipc *ipc, u32 i, u64 size) {
    if (i < ipc->nb && ipc->b[i].addr) return;   /* receive buffers need no reply descriptor */
    if (i < ipc->nc) {
        ipc->x_out[ipc->nx_out].addr = ipc->c[i].addr;
        ipc->x_out[ipc->nx_out].size = (u32)size;
        ipc->x_out[ipc->nx_out].index = i;
        ipc->nx_out++;
    }
}

void ipc_unknown(Ipc *ipc, IpcObject *self) {
    static struct { const char *name; u32 cmd; } seen[256];
    static u32 count;
    for (u32 i = 0; i < count; i++) if (seen[i].name == self->name && seen[i].cmd == ipc->command) return;
    if (count < 256) { seen[count].name = self->name; seen[count++].cmd = ipc->command; }
    fprintf(stderr, "[ipc] %s: command %u not implemented - succeeding with no output\n", self->name, ipc->command);
}

/* ---- the service manager --------------------------------------------------------- */
static void sm(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: return;   /* RegisterClient */
    case 1: {         /* GetServiceHandle(name: 8 characters) */
        char name[9] = {0};
        u64 raw = ipc_u64(ipc, 0);
        memcpy(name, &raw, 8);
        IpcObject *s = service_vi(name);
        if (!s) s = service_audio(name);
        if (!s) s = service_mii(name);
        if (!s) s = service_mm(name);
        if (!s) s = service_opus(name);
        if (!s) {
            fprintf(stderr, "[ipc] sm: service \"%s\" is not provided here\n", name);
            ipc->result = RESULT(21, 7);   /* not registered */
            return;
        }
        ipc_object(ipc, s);
        return;
    }
    }
    ipc_unknown(ipc, self);
}

void *ipc_connect(const char *port) {
    if (strcmp(port, "sm:")) return NULL;
    Session *s = calloc(1, sizeof *s);
    s->object = ipc_new_object("sm:", sm, NULL);
    return s;
}

void ipc_close(void *session) { (void)session; }   /* objects may still be shared through domains */

/* ---- the transport --------------------------------------------------------------- */
static u64 static_address(u32 d0, u32 d1) { return d1 | ((u64)((d0 >> 12) & 0xF) << 32) | ((u64)((d0 >> 6) & 0x3F) << 36); }

static IpcBuffer buffer_desc(const u32 *d) {
    IpcBuffer b;
    b.size = d[0] | ((u64)((d[2] >> 24) & 0xF) << 32);
    b.addr = d[1] | ((u64)((d[2] >> 28) & 0xF) << 32) | ((u64)((d[2] >> 2) & 0x3FFFFF) << 36);
    return b;
}

static u32 domain_add(Session *s, IpcObject *o) {
    for (u32 id = 1; id < 256; id++)
        if (!s->ids[id]) { s->ids[id] = o; return id; }
    rt_fatal("ipc: domain full");
}

u32 ipc_request(Cpu *c, void *session, u64 message, u64 size) {
    Session *s = session;
    u32 *m = P(u32, message);
    (void)size;
    Ipc *ipc = calloc(1, sizeof *ipc);
    ipc->cpu = c;
    u32 type = m[0] & 0xFFFF, nx = (m[0] >> 16) & 0xF, na = (m[0] >> 20) & 0xF, nb = (m[0] >> 24) & 0xF, nw = (m[0] >> 28) & 0xF;
    u32 raw_words = m[1] & 0x3FF, c_flags = (m[1] >> 10) & 0xF, p = 2;
    if (m[1] >> 31) {
        u32 sh = m[p++];
        if (sh & 1) p += 2;   /* process id */
        ipc->ncopy_in = (sh >> 1) & 0xF;
        ipc->nmove_in = (sh >> 5) & 0xF;
        for (u32 i = 0; i < ipc->ncopy_in; i++) ipc->copy_in[i] = m[p++];
        for (u32 i = 0; i < ipc->nmove_in; i++) ipc->move_in[i] = m[p++];
    }
    for (u32 i = 0; i < nx; i++, p += 2) {
        ipc->x[i].addr = static_address(m[p], m[p + 1]);
        ipc->x[i].size = m[p] >> 16;
    }
    ipc->nx = nx;
    for (u32 i = 0; i < na; i++, p += 3) ipc->a[i] = buffer_desc(&m[p]);
    ipc->na = na;
    for (u32 i = 0; i < nb; i++, p += 3) ipc->b[i] = buffer_desc(&m[p]);
    ipc->nb = nb;
    p += 3 * nw;
    u32 raw_start = p;
    if (c_flags >= 2) {
        u32 count = c_flags - 2 ? c_flags - 2 : 1;
        for (u32 i = 0; i < count && i < 8; i++) {
            u32 *d = &m[raw_start + raw_words + 2 * i];
            ipc->c[i].addr = d[0] | ((u64)(d[1] & 0xFFFF) << 32);
            ipc->c[i].size = d[1] >> 16;
        }
        ipc->nc = count < 8 ? count : 8;
    }
    u8 *raw = P(u8, (message + raw_start * 4 + 15) & ~15ull);
    int control = type == 5 || type == 7, domain_msg = s->domain && !control;
    IpcObject *target = s->object;
    u32 token = 0;

    if (type == 2) { free(ipc); return 0; }   /* close */
    if (domain_msg) {
        u8 dtype = raw[0], nobj = raw[1];
        u16 dsize = *(u16 *)(raw + 2);
        u32 oid = *(u32 *)(raw + 4);
        if (dtype == 2) {   /* close a domain object */
            if (oid < 256) s->ids[oid] = NULL;
            raw = NULL;
        } else {
            target = oid < 256 ? s->ids[oid] : NULL;
            ipc->nin_objects = nobj < 8 ? nobj : 8;
            for (u32 i = 0; i < ipc->nin_objects; i++) ipc->in_objects[i] = *(u32 *)(raw + 16 + dsize + 4 * i);
            raw += 16;
        }
    }
    if (raw) {
        if (*(u32 *)raw != SFCI) fprintf(stderr, "[ipc] %s: request without the command magic\n", target ? target->name : "?");
        ipc->command = *(u32 *)(raw + 8);
        token = *(u32 *)(raw + 12);
        ipc->in = raw + 16;
        ipc->in_size = raw_words * 4 > 32 ? raw_words * 4 - 32 : 0;

        AcquireSRWLockExclusive(&lock);
        if (control) {
            switch (ipc->command) {
            case 0:   /* ConvertCurrentObjectToDomain */
                if (!s->domain) { s->domain = 1; s->ids[1] = s->object; }
                IPC_PUSH(ipc, (u32)1);
                break;
            case 2: case 4:   /* CloneCurrentObject(Ex) */
                ipc->move_out[ipc->nmove_out++] = 0;   /* patched below with a new session */
                break;
            case 3: IPC_PUSH(ipc, (u16)0x8000); break;   /* QueryPointerBufferSize */
            default: fprintf(stderr, "[ipc] control command %u not implemented\n", ipc->command);
            }
        } else if (!target) {
            ipc->result = RESULT(10, 236);   /* no such object */
        } else {
            target->handle(target, ipc);
        }
        ReleaseSRWLockExclusive(&lock);
        if (rt_trace) fprintf(stderr, "[ipc] %s #%u -> %#x\n", control ? "control" : target ? target->name : "?", ipc->command, ipc->result);
    }

    /* ---- the response ---- */
    if (control && (ipc->command == 2 || ipc->command == 4)) {
        Session *clone = calloc(1, sizeof *clone);
        *clone = *s;
        ipc->move_out[0] = kernel_new_session(clone);
    }
    u32 objects_as_ids[8], nids = 0;
    for (u32 i = 0; i < ipc->nobjects_out; i++) {
        if (s->domain && !control) objects_as_ids[nids++] = domain_add(s, ipc->objects_out[i]);
        else {
            Session *n = calloc(1, sizeof *n);
            n->object = ipc->objects_out[i];
            ipc->move_out[ipc->nmove_out++] = kernel_new_session(n);
        }
    }
    u32 w = 0, *r = m;
    r[w++] = ipc->nx_out << 16;
    r[w++] = 0;
    if (ipc->ncopy_out || ipc->nmove_out) {
        r[1] |= 1u << 31;
        r[w++] = ipc->ncopy_out << 1 | ipc->nmove_out << 5;
        for (u32 i = 0; i < ipc->ncopy_out; i++) r[w++] = ipc->copy_out[i];
        for (u32 i = 0; i < ipc->nmove_out; i++) r[w++] = ipc->move_out[i];
    }
    for (u32 i = 0; i < ipc->nx_out; i++) {
        u64 a = ipc->x_out[i].addr;
        r[w++] = (ipc->x_out[i].index & 0x3F) | (u32)((a >> 36) & 0x3F) << 6 | (u32)((a >> 32) & 0xF) << 12 | ipc->x_out[i].size << 16;
        r[w++] = (u32)a;
    }
    u32 raw_at = w;
    u8 *out = P(u8, (message + w * 4 + 15) & ~15ull), *o = out;
    if (domain_msg) {   /* domain response header: number of objects returned */
        memset(o, 0, 16);
        *(u32 *)o = nids;
        o += 16;
    }
    *(u32 *)(o + 0) = SFCO;
    *(u32 *)(o + 4) = 0;
    *(u32 *)(o + 8) = ipc->result;
    *(u32 *)(o + 12) = token;
    memcpy(o + 16, ipc->out, ipc->out_size);
    o += 16 + ipc->out_size;
    for (u32 i = 0; i < nids; i++, o += 4) *(u32 *)o = objects_as_ids[i];
    u32 used = (u32)(o - P(u8, message + raw_at * 4));
    r[1] |= (used + 3) / 4;
    free(ipc);
    return 0;
}
