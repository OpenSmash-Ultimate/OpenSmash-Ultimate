/* Shared glyph lookup and atlas spans. Catalog glyphs reserve two cells so a
 * language can add wide characters without renderer-specific tables. */
#pragma once
#include <stddef.h>
#include "ui_catalog.generated.h"
#include "ui_utf8.h"

static int ui_font_cell(unsigned c) {
    if (c >= 32 && c < 127) return (int)c - 32;
    unsigned low = 0, high = UI_GLYPH_COUNT;
    while (low < high) {
        unsigned mid = low + (high - low) / 2;
        if (ui_glyphs[mid] < c) low = mid + 1; else high = mid;
    }
    return low < UI_GLYPH_COUNT && ui_glyphs[low] == c ? 96 + (int)low * 2 : '?' - 32;
}

static int ui_font_span(int cell) {
    return cell >= 96 ? ui_columns(ui_glyphs[(cell - 96) / 2]) : 1;
}
