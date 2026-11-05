#include "ui_list.h"

#include <string.h>

static const int kColX[3] = {700, 870, 1050};

static void draw_triangle(int x, int y, int size, SDL_Color c) {
    SDL_Vertex v[3] = {
        {{(float)x, (float)y}, c, {0, 0}},
        {{(float)x, (float)(y + size)}, c, {0, 0}},
        {{(float)(x + size * 0.87f), (float)(y + size / 2)}, c, {0, 0}},
    };
    SDL_RenderGeometry(ui_renderer(), NULL, v, 3, NULL, 0);
}

static void draw_icon(const row_t* r, int x, int y) {
    const int s = 64;
    switch (r->icon_kind) {
    case ICON_TEXTURE: {
        SDL_Rect dst = {x, y, s, s};
        SDL_RenderCopy(ui_renderer(), r->icon, NULL, &dst);
        break;
    }
    case ICON_PLAY:
        ui_round_rect(x, y, s, s, 12, UI_ACCENT);
        draw_triangle(x + 22, y + 16, 32, UI_TEXT);
        break;
    case ICON_PC:   /* a small monitor */
        ui_round_rect(x, y, s, s, 12, (SDL_Color){0x3a, 0x40, 0x47, 0xff});
        ui_round_rect(x + 12, y + 14, 40, 28, 4, UI_TEXT_DIM);
        ui_fill(x + 16, y + 18, 32, 20, (SDL_Color){0x2a, 0x6a, 0xb0, 0xff});
        ui_fill(x + 28, y + 42, 8, 6, UI_TEXT_DIM);
        ui_fill(x + 20, y + 48, 24, 3, UI_TEXT_DIM);
        break;
    case ICON_ADD:
        ui_round_rect(x, y, s, s, 12, (SDL_Color){0x2e, 0x7d, 0x4f, 0xff});
        ui_fill(x + 30, y + 16, 5, 32, UI_TEXT);
        ui_fill(x + 16, y + 30, 32, 5, UI_TEXT);
        break;
    case ICON_LETTER: {   /* no icon from the PC: first letter of the name */
        ui_round_rect(x, y, s, s, 12, (SDL_Color){0x3a, 0x40, 0x47, 0xff});
        char first[5] = {0};
        const unsigned char* t = (const unsigned char*)(r->title ? r->title : "?");
        int n = t[0] < 0x80 ? 1 : (t[0] & 0xE0) == 0xC0 ? 2 : (t[0] & 0xF0) == 0xE0 ? 3 : 4;
        memcpy(first, t, n);
        ui_text(x + s / 2, y + 10, first, 34, UI_TEXT, 0, UI_CENTER);
        break;
    }
    default:
        break;
    }
}

void draw_list(const row_t* rows, int count, int sel, int top, const char* const heads[4]) {
    /* Column headers, like the PC's game list. */
    ui_fill(0, 74, UI_W, 40, UI_BAR);
    ui_text(24, 82, heads[0], 20, UI_TEXT_DIM, 0, UI_LEFT);
    for (int c = 0; c < 3; c++)
        if (heads[c + 1]) ui_text(kColX[c], 82, heads[c + 1], 20, UI_TEXT_DIM, 0, UI_LEFT);

    for (int i = 0; i < VISIBLE_ROWS && top + i < count; i++) {
        const row_t* r = &rows[top + i];
        const int y = LIST_Y + i * ROW_H;
        const bool selected = top + i == sel;
        ui_fill(0, y, UI_W, ROW_H, selected ? UI_ROW_SEL : ((top + i) % 2 ? UI_ROW_B : UI_ROW_A));
        if (selected) ui_fill(0, y, 6, ROW_H, UI_ACCENT);
        draw_icon(r, 24, y + 12);
        const int tw = ui_text(108, y + (r->subtitle ? 14 : 28), r->title, 26, UI_TEXT, 470, UI_LEFT);
        if (r->subtitle) ui_text(108, y + 50, r->subtitle, 18, UI_TEXT_DIM, 560, UI_LEFT);
        if (r->badge) {
            const int bw = ui_text_width(r->badge, 16) + 20;
            const int bx = 108 + tw + 14;
            if (bx + bw < kColX[0] - 10) {
                ui_round_rect(bx, y + 18, bw, 26, 13, r->badge_color);
                ui_text(bx + bw / 2, y + 21, r->badge, 16, UI_BG, 0, UI_CENTER);
            }
        }
        for (int c = 0; c < 3; c++)
            if (r->col[c]) ui_text(kColX[c], y + 31, r->col[c], 20, UI_TEXT, 170, UI_LEFT);
    }
    /* Scrollbar when the list is longer than the screen. */
    if (count > VISIBLE_ROWS) {
        const int track = VISIBLE_ROWS * ROW_H;
        const int h = track * VISIBLE_ROWS / count;
        const int y = LIST_Y + (track - h) * top / (count - VISIBLE_ROWS);
        ui_round_rect(UI_W - 10, y, 6, h, 3, UI_TEXT_DIM);
    }
}

bool list_input(uint64_t down, int count, int* sel, int* top) {
    if (count <= 0) return false;
    if (down & HidNpadButton_AnyDown) *sel = (*sel + 1) % count;
    if (down & HidNpadButton_AnyUp) *sel = (*sel + count - 1) % count;
    if (down & (HidNpadButton_AnyRight | HidNpadButton_R)) *sel = *sel + VISIBLE_ROWS < count ? *sel + VISIBLE_ROWS : count - 1;
    if (down & (HidNpadButton_AnyLeft | HidNpadButton_L)) *sel = *sel > VISIBLE_ROWS ? *sel - VISIBLE_ROWS : 0;
    bool activate = (down & HidNpadButton_A) != 0;
    int tx, ty;
    if (ui_tap(&tx, &ty) && ty >= LIST_Y && ty < LIST_Y + VISIBLE_ROWS * ROW_H) {
        const int row = *top + (ty - LIST_Y) / ROW_H;
        if (row < count) {
            if (row == *sel) activate = true;
            *sel = row;
        }
    }
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + VISIBLE_ROWS) *top = *sel - VISIBLE_ROWS + 1;
    return activate;
}

