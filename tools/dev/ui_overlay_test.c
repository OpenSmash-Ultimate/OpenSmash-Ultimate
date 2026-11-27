/* Real Windows overlay atlas and quads, without a device, window, or game. */
#include <assert.h>
#include <math.h>
#include "../../runtime/gpu_overlay.c"

int menu_visible(void) { return 0; }
int menu_blocks_input(void) { return 0; }
void menu_draw(float w, float h) { (void)w; (void)h; }
int game_fight_on(void) { return 0; }
int net_peer_quality(NetPeerQ *out, int max) { (void)out; (void)max; return 0; }
static int ping_on = 1, ping_fight;   /* (the ping overlay: display.ini net_ping, an online rollback fight of 2 players) */
int display_net_ping(void) { return ping_on; }
int rb_net_ping_view(NetPingView *v) {
    memset(v, 0, sizeof *v);
    if (!ping_fight) return 0;
    v->players = 2; v->local = 0; v->ms[0] = -2; v->ms[1] = 48; v->delay = 2; v->delay_how = NET_DELAY_AUTO;
    return 2;
}

int main(void) {
    unsigned w, h;
    const unsigned char *pixels = overlay_coverage(&w, &h);
    assert(pixels && w == ATLAS_W && h == ATLAS_H);
    unsigned at = 0;
    while (at < UI_GLYPH_COUNT && ui_glyphs[at] != 0x754c) at++; /* the UI's word for interface */
    assert(at < UI_GLYPH_COUNT);
    unsigned cell = 96 + at * 2, ink = 0;
    for (unsigned y = 0; y < CELL_H; y++)
        for (unsigned x = 0; x < CELL_W * 2; x++) ink += pixels[(cell / COLS * CELL_H + y) * w + cell % COLS * CELL_W + x] > 128;
    assert(ink > 100);
    /* The new scripts and Latin accents must have ink in their own atlas cells. */
    const unsigned examples[] = {0x8a2d, 0xc124, 0x00e8, 0x00f1, 0x00fc};
    for (unsigned j = 0; j < sizeof examples / sizeof *examples; j++) {
        int glyph = ui_font_cell(examples[j]);
        assert(glyph >= 96);
        unsigned ink = 0;
        for (unsigned y = 0; y < CELL_H; y++)
            for (unsigned x = 0; x < CELL_W * ui_font_span(glyph); x++)
                ink += pixels[(glyph / COLS * CELL_H + y) * w + glyph % COLS * CELL_W + x] > 128;
        assert(ink > 20);
    }
    nverts = 0;
    ovl_text(0, 0, 40, 0xffffffff, "A\347\225\214B");
    assert(nverts == 36);
    assert(verts[6].x == 0 && verts[18].x == 20 && verts[30].x == 60);
    assert(verts[19].x == 60); /* CJK occupies twice an ASCII letter's width. */
    nverts = 0;
    ovl_text(0, 0, 40, 0xffffffff, "A\342\200\246B");
    /* narrow Unicode samples one cell, less half a texel in from each side (gpu_overlay.c: the linear filter never reads the next cell) */
    assert(fabsf((verts[19].u - verts[18].u) - (1.0f / COLS - 1.0f / ATLAS_W)) < 1e-6f);
    assert(verts[30].x == 40);
    /* the ping in an online rollback fight: by player 2's stock icons at 1920x1080 (right of x 380, above y 1060), none
     * outside one, and none with the switch off (nothing asked: the view is not even read) */
    const OverlayVertex *ov;
    assert(overlay_quads(1920, 1080, 0, NULL, &ov) == 0);
    ping_fight = 1; Sleep(520);   /* (the view is read twice a second) */
    unsigned nq = overlay_quads(1920, 1080, 0, NULL, &ov);
    assert(nq > 0 && nq % 6 == 0);
    float lo = 1e9f, hi = 0, top = 1e9f, bottom = 0;
    for (unsigned i = 0; i < nq; i++) {
        if (ov[i].x < lo) lo = ov[i].x; if (ov[i].x > hi) hi = ov[i].x;
        if (ov[i].y < top) top = ov[i].y; if (ov[i].y > bottom) bottom = ov[i].y;
    }
    assert(lo > 1000 && hi < 1210 && top > 1000 && bottom < 1080);
    ping_on = 0;
    assert(overlay_quads(1920, 1080, 0, NULL, &ov) == 0);
    puts("Windows UTF-8 overlay atlas and quads passed");
    return 0;
}
