/* Real Windows overlay atlas and quads, without a device, window, or game. */
#include <assert.h>
#include "../../runtime/gpu_overlay.c"

int menu_visible(void) { return 0; }
int menu_blocks_input(void) { return 0; }
void menu_draw(float w, float h) { (void)w; (void)h; }
int game_fight_on(void) { return 0; }
int net_peer_quality(NetPeerQ *out, int max) { (void)out; (void)max; return 0; }

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
    assert(verts[19].u - verts[18].u == 1.0f / COLS); /* narrow Unicode samples one cell */
    assert(verts[30].x == 40);
    puts("Windows UTF-8 overlay atlas and quads passed");
    return 0;
}
