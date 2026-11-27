/* The debug overlay: frame rate, a graph of recent frame times and the renderer's statistics,
 * drawn in the corner of each presented frame; and, while it is open, the settings menu
 * (gpu_menu.c), with the same quads.
 *
 * Text comes from a glyph atlas GDI draws once (a monospaced font, printable ASCII, plus a
 * solid cell for rectangles), at twice the overlay's own text size so the menu's larger text
 * stays sharp; everything is quads from one dynamic vertex buffer. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include "gpu_overlay.h"
#include "gpu_menu.h"
#include "display.h"
#include "net.h"
#include "net_ping.h"

#include "ui_font.h"

#define CELL_W 24                 /* the atlas's cells */
#define CELL_H 48
#define TEXT_W 12                 /* the overlay's own letters: half size */
#define TEXT_H 24
#define COLS UI_FONT_COLS
#define ATLAS_W (CELL_W * COLS)
#define ATLAS_H (CELL_H * UI_FONT_ROWS)      /* ' '..'~', then the solid cell (127) */
#define MAX_QUADS 8192
#define HISTORY 240

typedef struct Vertex { float x, y, u, v; unsigned color; } Vertex;

static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vbuf, *cbuf;
static ID3D11ShaderResourceView *atlas;
static ID3D11SamplerState *sampler;
static ID3D11BlendState *blend;
static ID3D11RasterizerState *raster;
static int state;   /* 0 not yet, 1 ready, -1 failed */

static Vertex verts[MAX_QUADS * 6];
static unsigned nverts;
static const void *verts_out(void) { return verts; }

static const char HLSL[] =
    "cbuffer C : register(b0) { float2 inv_size; };\n"
    "struct I { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "O vs(I i) { O o; o.p = float4(i.p * inv_size * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(O i) : SV_Target { return float4(i.c.rgb, i.c.a * t.Sample(s, i.uv).r); }\n";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(HLSL, sizeof HLSL - 1, "overlay", NULL, NULL, entry, target, 0, 0, &code, &errors))) {
        if (errors) { fprintf(stderr, "[overlay] %s\n", (const char *)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
        return NULL;
    }
    return code;
}

/* The glyphs: drawn white on black by GDI, kept as coverage in a one-channel texture. (Linux:
 * the same drawing, done at build time on Windows - tools/linux/make_overlay_font.c.) */
#ifndef _WIN32
#include "overlay_font.h"
#endif
static unsigned char coverage[ATLAS_W * ATLAS_H];
static int make_coverage(void) {
    static int made;   /* 0 not yet, 1 made, -1 failed */
    if (made) return made > 0;
    made = -1;
#ifdef _WIN32
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader = (BITMAPINFOHEADER){sizeof(BITMAPINFOHEADER), ATLAS_W, -ATLAS_H, 1, 32, BI_RGB};
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(dc); return 0; }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    HFONT font = CreateFontW(-40, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    for (int c = 32; c < 127; c++) {
        char ch = (char)c;
        TextOutA(dc, (c - 32) % COLS * CELL_W, (c - 32) / COLS * CELL_H + 2, &ch, 1);
    }
    HFONT unicode_font = CreateFontW(-38, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT japanese_font = CreateFontW(-38, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    HFONT korean_font = CreateFontW(-38, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
    for (unsigned i = 0; i < UI_GLYPH_COUNT; i++) {
        unsigned cp = ui_glyphs[i], cell = 96 + i * 2;
        SelectObject(dc, cp >= 0xac00 && cp <= 0xd7af ? korean_font :
            cp >= 0x3040 && cp <= 0x30ff ? japanese_font : ui_columns(cp) > 1 ? unicode_font : font);
        WCHAR chars[2]; int count = 1;
        if (cp > 0xffff) { cp -= 0x10000; chars[0] = (WCHAR)(0xd800 + (cp >> 10)); chars[1] = (WCHAR)(0xdc00 + (cp & 1023)); count = 2; }
        else chars[0] = (WCHAR)cp;
        TextOutW(dc, cell % COLS * CELL_W, cell / COLS * CELL_H + 2, chars, count);
    }
    GdiFlush();
    const unsigned *px = bits;
    for (int i = 0; i < ATLAS_W * ATLAS_H; i++) coverage[i] = (unsigned char)(px[i] >> 16);   /* red */
#else
    memcpy(coverage, overlay_font, sizeof coverage);
#endif
    int solid = 127 - 32;   /* (filled away from its edges: scaled-down text samples next to its cell, and 'o' is above) */
    for (int y = 4; y < CELL_H - 4; y++) memset(coverage + (solid / COLS * CELL_H + y) * ATLAS_W + solid % COLS * CELL_W + 4, 255, CELL_W - 8);
#ifdef _WIN32
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(japanese_font);
    DeleteObject(korean_font);
    DeleteObject(unicode_font);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
#endif
    made = 1;
    return 1;
}
static int make_atlas(ID3D11Device *dev) {
    if (!make_coverage()) return 0;
    D3D11_TEXTURE2D_DESC td = {ATLAS_W, ATLAS_H, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {coverage, ATLAS_W, 0};
    ID3D11Texture2D *tex;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) return 0;
    HRESULT hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &atlas);
    ID3D11Texture2D_Release(tex);
    return SUCCEEDED(hr);
}

static int init(ID3D11Device *dev) {
    ID3DBlob *v = compile("vs", "vs_5_0"), *p = compile("ps", "ps_5_0");
    if (!v || !p) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, el, 3, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), &layout);
    ID3D10Blob_Release(v);
    ID3D10Blob_Release(p);
    D3D11_BUFFER_DESC vb = {sizeof verts, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    D3D11_BUFFER_DESC cb = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &vb, NULL, &vbuf);
    ID3D11Device_CreateBuffer(dev, &cb, NULL, &cbuf);
    D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, 0};
    ID3D11Device_CreateSamplerState(dev, &sd, &sampler);
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                                          D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 15};
    ID3D11Device_CreateBlendState(dev, &bd, &blend);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(dev, &rd, &raster);
    return vs && ps && layout && vbuf && cbuf && sampler && blend && raster && make_atlas(dev);
}

/* ---- drawing ------------------------------------------------------------------------- */
static void quad(float x, float y, float w, float h, int cell, unsigned color) {
    if (nverts + 6 > MAX_QUADS * 6) return;
    float u0 = (float)(cell % COLS * CELL_W) / ATLAS_W, v0 = (float)(cell / COLS * CELL_H) / ATLAS_H;
    float u1 = u0 + (float)(CELL_W * ui_font_span(cell)) / ATLAS_W, v1 = v0 + (float)CELL_H / ATLAS_H;
    if (cell == 127 - 32) { u0 = u1 = u0 + 0.5f * CELL_W / ATLAS_W; v0 = v1 = v0 + 0.5f * CELL_H / ATLAS_H; }   /* solid: its middle */
    else { u0 += 0.5f / ATLAS_W; u1 -= 0.5f / ATLAS_W; v0 += 0.5f / ATLAS_H; v1 -= 0.5f / ATLAS_H; }   /* a letter: half a texel in, so the linear filter never reads the next cell (a 'g' above a 'w', ...) */
    Vertex a = {x, y, u0, v0, color}, b = {x + w, y, u1, v0, color}, c = {x, y + h, u0, v1, color}, d = {x + w, y + h, u1, v1, color};
    Vertex *o = &verts[nverts];
    o[0] = a; o[1] = b; o[2] = c; o[3] = c; o[4] = b; o[5] = d;
    nverts += 6;
}
static void rect(float x, float y, float w, float h, unsigned color) { quad(x, y, w, h, 127 - 32, color); }

#define RGBA OVL_RGBA

/* Text at (x, y), letters size high, one line per '\n'; returns the y below it. */
static float text_sized(float x, float y, float size, unsigned color, const char *s) {
    float cx = x, w = size / 2, shadow = size >= 32 ? 2.0f : 1.0f;
    while (*s) {
        unsigned c = ui_utf8_next(&s);
        if (c == '\n') { cx = x; y += size; continue; }
        int columns = ui_columns(c), cell = ui_font_cell(c);
        if (c > 32 && columns) {
            quad(cx + shadow, y + shadow, w * columns, size, cell, RGBA(0, 0, 0, 200));
            quad(cx, y, w * columns, size, cell, color);
        }
        cx += w * columns;
    }
    return y + size;
}

static float text(float x, float y, unsigned color, const char *s) { return text_sized(x, y, TEXT_H, color, s); }

void ovl_rect(float x, float y, float w, float h, unsigned color) { rect(x, y, w, h, color); }
float ovl_text(float x, float y, float size, unsigned color, const char *s) { return text_sized(x, y, size, color, s); }

static int lines_of(const char *s, int *widest) {
    int n = 1, w = 0, cur = 0;
    for (; *s; s++) { if (*s == '\n') { n++; cur = 0; } else if (++cur > w) w = cur; }
    *widest = w;
    return n;
}

/* A message in the middle of the picture (the start-up shader preparation): text and a bar. */
static char message[160];
static float message_progress = -1;
void overlay_message(const char *text, float progress) {
    if (!text || !*text) { message[0] = 0; message_progress = -1; return; }
    snprintf(message, sizeof message, "%s", text);
    message_progress = progress;
}

static void message_draw(float w, float h) {
    if (!message[0]) return;
    int widest = 0, lines = lines_of(message, &widest);
    float size = 30, tw = widest * size / 2;
    float bw = tw + 64;
    if (bw < 520) bw = 520;
    if (bw > w - 40) bw = w - 40;
    float bh = 20 + lines * size + (message_progress >= 0 ? 34 : 0);
    float x = (w - bw) / 2, y = (h - bh) / 2;
    rect(x, y, bw, bh, RGBA(0, 0, 0, 210));
    rect(x, y, bw, 2, RGBA(120, 200, 255, 255));
    float ty = text_sized(x + 24, y + 20, size, RGBA(240, 240, 240, 255), message);
    if (message_progress >= 0) {
        float p = message_progress < 0 ? 0 : message_progress > 1 ? 1 : message_progress;
        float bx = x + 24, by = ty + 8, bw2 = bw - 48, bh2 = 16;
        rect(bx, by, bw2, bh2, RGBA(40, 40, 40, 255));
        rect(bx, by, bw2 * p, bh2, RGBA(90, 200, 120, 255));
    }
}

/* ---- the frame ---------------------------------------------------------------------- */
static void hud(const char *detail);
int game_fight_on(void);   /* (hle_fight_test.c) */

/* The other players' ping in a room, top right, outside fights (on the character select, the stage vote, the
 * results): a dot coloured by the connection's quality and the round trip. After saad-script's
 * ssbu-online-deluxe (its idea; nothing taken from its code). Returns 1 when something is to be drawn. */
static NetPeerQ badge_peers[4];
static int badge_n;
static int badge_wanted(void) {
    static u64 next;
    if (!display_net_ping()) return 0;   /* (SSBU_NET_PING=0, display.ini net_ping = 0: no ping on screen) */
    u64 t = GetTickCount64();
    if (t >= next) {   /* (twice a second) */
        int was = badge_n;
        next = t + 500;
        badge_n = net_peer_quality(badge_peers, 4);
        if (badge_n && !was)
            fprintf(stderr, "[net] ping badge: %d player%s (%s: %d ms%s, quality %d)\n", badge_n, badge_n == 1 ? "" : "s", badge_peers[0].name,
                    badge_peers[0].rtt_ms, badge_peers[0].relayed ? ", relayed" : "", badge_peers[0].quality);
    }
    return badge_n > 0 && !game_fight_on();
}
static float text_sized(float x, float y, float size, unsigned color, const char *s);
static void badge(float w) {
    static const unsigned dot[3] = {OVL_RGBA(90, 220, 110, 255), OVL_RGBA(250, 210, 70, 255), OVL_RGBA(240, 80, 70, 255)};
    float size = 18, row = size + 6, bw = 300, x = w - bw - 12, y = 12;
    rect(x - 8, y - 6, bw + 8, row * badge_n + 6, OVL_RGBA(0, 0, 0, 150));
    for (int i = 0; i < badge_n; i++) {
        const NetPeerQ *p = &badge_peers[i];
        char line[96];
        if (p->rtt_ms >= 0) snprintf(line, sizeof line, "%.24s  %d ms", p->name, p->rtt_ms);
        else snprintf(line, sizeof line, "%.24s  relayed", p->name);
        int q = p->quality < 0 ? 0 : p->quality > 2 ? 2 : p->quality;
        rect(x, y + row * i + 4, size - 6, size - 6, dot[q]);
        text_sized(x + size + 2, y + row * i, size, OVL_RGBA(235, 235, 235, 255), line);
    }
}
/* The ping in an online rollback fight (net_ping.h has the rules; SSBU Arena's look): each other player's round trip and
 * the input delay in use, by a 2-player fight's stock icons or in a list at the top left. The numbers and their text are
 * taken twice a second (rb_net_ping_view); a frame only makes the quads from that text: nothing is formatted or
 * allocated per frame. Off (SSBU_NET_PING=0, display.ini net_ping = 0): nothing is asked or drawn. */
int rb_net_ping_view(NetPingView *v);   /* (hle_rb_core.c) */
static struct {
    int n, local, ms[NET_PING_PLAYERS], tlen[NET_PING_PLAYERS], dlen;
    char t[NET_PING_PLAYERS][8], delay[16];
} fp;
static int fight_ping_wanted(void) {
    static u64 next;
    if (!display_net_ping()) return 0;
    u64 t = GetTickCount64();
    if (t >= next) {   /* (twice a second) */
        static char said[16] = "-";
        NetPingView v;
        int was = fp.n;
        next = t + 500;
        fp.n = rb_net_ping_view(&v);
        if (fp.n) {
            fp.local = v.local;
            for (int i = 0; i < fp.n; i++) { fp.ms[i] = v.ms[i]; net_ping_text(fp.t[i], sizeof fp.t[i], v.ms[i]); fp.tlen[i] = (int)strlen(fp.t[i]); }
            net_ping_delay_text(fp.delay, sizeof fp.delay, v.delay, v.delay_how);
            fp.dlen = (int)strlen(fp.delay);
            if (!was || strcmp(said, fp.delay)) {   /* (a line as the fight's overlay starts, and when the delay changes) */
                char pings[96] = "";
                size_t k = 0;
                for (int i = 0; i < fp.n && k + 16 < sizeof pings; i++)
                    if (v.ms[i] != -2) k += (size_t)snprintf(pings + k, sizeof pings - k, "%sP%d %s", k ? ", " : "", i + 1, fp.t[i]);
                fprintf(stderr, "[net] ping overlay: in the fight: %d players, this game P%d; pings %s; input delay %s\n", fp.n, fp.local + 1,
                        pings[0] ? pings : "none", fp.delay[0] ? fp.delay : "not chosen yet");
                snprintf(said, sizeof said, "%s", fp.delay);
            }
        }
    }
    return fp.n > 0;
}
#define PING_DARK OVL_RGBA(15, 15, 15, 255)
#define PING_LIGHT OVL_RGBA(235, 235, 235, 255)
static const unsigned ping_colour[3] = {OVL_RGBA(77, 219, 89, 255), OVL_RGBA(250, 204, 46, 255), OVL_RGBA(242, 66, 56, 255)};
#define PING_UNLIT OVL_RGBA(107, 107, 115, 255)
#define PING_CAP 0.32f   /* (the atlas's capitals start this far down a letter's height, 0.54 of it high: measured on its glyphs) */
static void ping_glyphs(float x, float y, float size, unsigned color, const char *s) {   /* (text_sized without its shadow) */
    for (float w = size / 2; *s; s++, x += w)
        if (*s > 32 && *s < 127) quad(x, y, w, size, *s - 32, color);
}
/* bold, with a dark outline: two passes half a dot apart over dark copies around (SSBU Arena's ldn_ping_text) */
static void ping_text(float x, float y, float size, float o, unsigned color, const char *s) {
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 2; dx++)
            if (dx || dy) ping_glyphs(x + dx * o, y + dy * o, size, PING_DARK, s);
    ping_glyphs(x, y, size, color, s);
    ping_glyphs(x + o, y, size, color, s);
}
/* one item at (x, y), the top of its icon (7 dots high, as the capitals): four bars, the number, the delay */
static void ping_item(float x, float y, float h, int ms, const char *t, int tlen, const char *df) {
    float dot = net_ping_dot(h), gap = dot / 2 > 1 ? dot / 2 : 1, o = gap, h7 = 7 * dot, size = net_ping_size(h);
    int level = net_ping_level(ms), lit = net_ping_bars(ms);
    for (int i = 0; i < 4; i++) {
        float bx = x + i * (dot + gap), bh = h7 * (i + 1) / 4;
        rect(bx - o, y + h7 - bh - o, dot + 2 * o, bh + 2 * o, PING_DARK);
    }
    for (int i = 0; i < 4; i++) {
        float bx = x + i * (dot + gap), bh = h7 * (i + 1) / 4;
        rect(bx, y + h7 - bh, dot, bh, i < lit ? ping_colour[level] : PING_UNLIT);
    }
    float tx = x + 4 * dot + 3 * gap + 2 * dot, ty = y - PING_CAP * size;
    ping_text(tx, ty, size, o, ping_colour[level], t);
    if (df[0]) ping_text(tx + tlen * size / 2 + 3 * dot, ty, size, o, PING_LIGHT, df);
}
static void fight_ping(float w, float h) {
    float dot = net_ping_dot(h), size = net_ping_size(h);
    if (fp.n == 2) {   /* left of the other player's stock icons */
        int i = 1 - fp.local;
        float right, bottom;
        net_ping_anchor(w, h, i, &right, &bottom);
        ping_item(right - net_ping_width(h, fp.tlen[i], fp.dlen), bottom - 7 * dot, h, fp.ms[i], fp.t[i], fp.tlen[i], fp.delay);
        return;
    }
    /* 3 or more players: one row at the top's middle ("P2 <bars> 48   P3 <bars> 60   2f auto"; the frame-rate overlay
     * has the top left, the timer the top right) */
    float o = dot / 2 > 1 ? dot / 2 : 1, cw = size / 2, label = 3 * cw, sep = 4 * cw, total = 0;
    for (int i = 0; i < fp.n; i++) if (fp.ms[i] != -2) total += (total > 0 ? sep : 0) + label + net_ping_width(h, fp.tlen[i], 0);
    if (fp.dlen) total += sep + fp.dlen * cw;
    float x = (w - total) / 2, y = h / 60 + PING_CAP * size;
    int first = 1;
    for (int i = 0; i < fp.n; i++) {
        if (fp.ms[i] == -2) continue;
        char p[4] = {'P', (char)('1' + i), 0, 0};
        if (!first) x += sep;
        first = 0;
        ping_text(x, y - PING_CAP * size, size, o, PING_LIGHT, p);
        ping_item(x + label, y, h, fp.ms[i], fp.t[i], fp.tlen[i], "");
        x += label + net_ping_width(h, fp.tlen[i], 0);
    }
    if (fp.dlen) ping_text(x + sep, y - PING_CAP * size, size, o, PING_LIGHT, fp.delay);
}
static void draw(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w, unsigned h);
static double history[HISTORY];   /* milliseconds per frame, newest at [head - 1] */
static unsigned head, filled;

void overlay_present(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w,
                     unsigned h, int show, const char *detail) {
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        history[head] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        head = (head + 1) % HISTORY;
        if (filled < HISTORY) filled++;
    }
    last = now;
    int menu = menu_visible(), ping = !menu && badge_wanted(), fping = !menu && fight_ping_wanted(), msg = message[0] != 0;
    if (!show && !menu && !ping && !fping && !msg) return;
    if (!state) state = init(dev) ? 1 : -1;
    if (state < 0) return;
    nverts = 0;
    if (show && !menu_blocks_input()) hud(detail);   /* (an open menu instead of the overlay) */
    if (ping) badge((float)w);
    if (fping) fight_ping((float)w, (float)h);
    if (menu) menu_draw((float)w, (float)h);
    if (msg) message_draw((float)w, (float)h);
    draw(ctx, rtv, x0, y0, w, h);
}

/* For another renderer (the Vulkan one, runtime/gpu_vk.c): the same frame-time bookkeeping and the same
 * quads as overlay_present, handed over instead of drawn. Returns the number of vertices (triangle list,
 * in the picture's pixels: x, y, atlas u, v, colour 0xAABBGGRR), 0 when nothing is to be drawn. */
unsigned overlay_quads(unsigned w, unsigned h, int show, const char *detail, const OverlayVertex **verts) {
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        history[head] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        head = (head + 1) % HISTORY;
        if (filled < HISTORY) filled++;
    }
    last = now;
    int menu = menu_visible(), ping = !menu && badge_wanted(), fping = !menu && fight_ping_wanted(), msg = message[0] != 0;
    *verts = (const OverlayVertex *)verts_out();
    if (!show && !menu && !ping && !fping && !msg) return 0;
    nverts = 0;
    if (show && !menu_blocks_input()) hud(detail);
    if (ping) badge((float)w);
    if (fping) fight_ping((float)w, (float)h);
    if (menu) menu_draw((float)w, (float)h);
    if (msg) message_draw((float)w, (float)h);
    return nverts;
}

/* The glyph atlas's coverage (one byte a texel, w x h), or NULL if it cannot be made. */
const unsigned char *overlay_coverage(unsigned *w, unsigned *h) {
    *w = ATLAS_W;
    *h = ATLAS_H;
    return make_coverage() ? coverage : NULL;
}

/* The overlay: the last second's frame rate and a graph of the last 240 frames' times. */
static void hud(const char *detail) {
    /* the last second: frames, slowest and fastest */
    double total = 0, worst = 0, best = 1e9;
    unsigned frames = 0;
    for (unsigned i = 0; i < filled && total < 1000.0; i++) {
        double ms = history[(head + HISTORY - 1 - i) % HISTORY];
        total += ms; frames++;
        if (ms > worst) worst = ms;
        if (ms < best) best = ms;
    }
    double fps = total > 0 ? frames * 1000.0 / total : 0, last_ms = filled ? history[(head + HISTORY - 1) % HISTORY] : 0;
    char line[256];
    float x = 16, y = 16, gw = 2.0f * HISTORY, gh = 100;   /* graph: 2 px a frame; 100 px = 50 ms */
    int detail_w = 0, detail_lines = detail ? lines_of(detail, &detail_w) : 0;
    float panel_w = gw + 16, panel_h = 2 * TEXT_H + 8 + gh + 12 + detail_lines * TEXT_H;
    if (detail_w * TEXT_W + 16 > panel_w) panel_w = (float)(detail_w * TEXT_W + 16);
    rect(x - 8, y - 6, panel_w, panel_h, RGBA(0, 0, 0, 150));
    unsigned fps_color = fps >= 57 ? RGBA(120, 255, 120, 255) : fps >= 45 ? RGBA(255, 230, 90, 255) : RGBA(255, 100, 90, 255);
    snprintf(line, sizeof line, "%5.1f FPS  %5.1f ms", fps, last_ms);
    y = text(x, y, fps_color, line);
    snprintf(line, sizeof line, "last second: %.1f-%.1f ms", frames ? best : 0, worst);
    y = text(x, y, RGBA(230, 230, 230, 255), line);
    y += 6;
    /* the graph: a bar per frame, its height its time; lines at 60 and 30 frames a second */
    float scale = gh / 50.0f;
    rect(x, y, gw, gh, RGBA(20, 20, 20, 160));
    for (unsigned i = 0; i < filled; i++) {
        double ms = history[(head + HISTORY - filled + i) % HISTORY];
        float bh = (float)(ms > 50 ? 50 : ms) * scale;
        unsigned c = ms <= 17.5 ? RGBA(90, 220, 90, 230) : ms <= 34 ? RGBA(240, 210, 60, 230) : RGBA(240, 70, 60, 240);
        rect(x + gw - 2.0f * (filled - i), y + gh - bh, 2, bh, c);
    }
    rect(x, y + gh - 16.67f * scale, gw, 1, RGBA(255, 255, 255, 140));
    rect(x, y + gh - 33.33f * scale, gw, 1, RGBA(255, 255, 255, 70));
    y += gh + 6;
    if (detail) text(x, y, RGBA(210, 225, 255, 255), detail);
}

/* Every queued quad, over the picture at (x0, y0), w x h. */
static void draw(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, unsigned x0, unsigned y0, unsigned w, unsigned h) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, verts, nverts * sizeof *verts);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vbuf, 0);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float inv[4] = {1.0f / (float)w, 1.0f / (float)h, 0, 0};
        memcpy(m.pData, inv, sizeof inv);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbuf, 0);
    }
    UINT stride = sizeof(Vertex), offset = 0;
    D3D11_VIEWPORT vp = {(float)x0, (float)y0, (float)w, (float)h, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, raster);
    ID3D11DeviceContext_OMSetBlendState(ctx, blend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &atlas);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sampler);
    ID3D11DeviceContext_Draw(ctx, nverts, 0);
    ID3D11Buffer *none_buf = NULL;
    ID3D11ShaderResourceView *none_srv = NULL;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &none_buf, &stride, &offset);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none_srv);
}
