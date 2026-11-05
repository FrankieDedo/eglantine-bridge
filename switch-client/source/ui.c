#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_ttf.h>

static SDL_Window* g_win;
static SDL_Renderer* g_ren;
static PlFontData g_font_std, g_font_ext;

/* Fonts are opened per size on first use: the standard font for text, the
 * Nintendo extension font for button glyphs (private use area). */
#define MAX_FONTS 8
static struct {
    int size;
    TTF_Font* std;
    TTF_Font* ext;
} g_fonts[MAX_FONTS];

/* Rendered strings are kept as textures: drawing a list every frame would
 * otherwise rasterize the same text 60 times a second. */
#define CACHE_SIZE 256
typedef struct {
    char text[160];
    int size;
    uint32_t color;
    SDL_Texture* tex;
    int w, h;
    uint32_t used;
} cache_entry_t;
static cache_entry_t g_cache[CACHE_SIZE];
static uint32_t g_frame;

SDL_Window* ui_window(void) { return g_win; }
SDL_Renderer* ui_renderer(void) { return g_ren; }

bool ui_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    g_win = SDL_CreateWindow("Eglantine Bridge", 0, 0, UI_W, UI_H, 0);
    if (!g_win) return false;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) return false;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    if (TTF_Init() != 0) return false;
    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    if (R_FAILED(plGetSharedFontByType(&g_font_std, PlSharedFontType_Standard))) return false;
    plGetSharedFontByType(&g_font_ext, PlSharedFontType_NintendoExt);
    return true;
}

void ui_exit(void) {
    for (int i = 0; i < CACHE_SIZE; i++)
        if (g_cache[i].tex) SDL_DestroyTexture(g_cache[i].tex);
    for (int i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].std) TTF_CloseFont(g_fonts[i].std);
        if (g_fonts[i].ext) TTF_CloseFont(g_fonts[i].ext);
    }
    TTF_Quit();
    plExit();
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

static TTF_Font* font_for(int size, bool ext) {
    int free_slot = -1;
    for (int i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].size == size) return ext ? g_fonts[i].ext : g_fonts[i].std;
        if (!g_fonts[i].size && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    g_fonts[free_slot].size = size;
    g_fonts[free_slot].std = TTF_OpenFontRW(SDL_RWFromConstMem(g_font_std.address, g_font_std.size), 1, size);
    if (g_font_ext.address)
        g_fonts[free_slot].ext =
            TTF_OpenFontRW(SDL_RWFromConstMem(g_font_ext.address, g_font_ext.size), 1, size);
    return ext ? g_fonts[free_slot].ext : g_fonts[free_slot].std;
}

/* Strings made of glyphs from the private use area go to the extension font. */
static bool is_glyph_text(const char* s) { return (unsigned char)s[0] == 0xEE; }

static cache_entry_t* render_cached(const char* text, int size, SDL_Color c) {
    const uint32_t color = (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | c.a;
    int oldest = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        cache_entry_t* e = &g_cache[i];
        if (e->tex && e->size == size && e->color == color && !strcmp(e->text, text)) {
            e->used = g_frame;
            return e;
        }
        if (g_cache[i].used < g_cache[oldest].used || !g_cache[i].tex) oldest = i;
    }
    TTF_Font* f = font_for(size, is_glyph_text(text));
    if (!f) return NULL;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, text, c);
    if (!surf) return NULL;
    cache_entry_t* e = &g_cache[oldest];
    if (e->tex) SDL_DestroyTexture(e->tex);
    e->tex = SDL_CreateTextureFromSurface(g_ren, surf);
    e->w = surf->w;
    e->h = surf->h;
    SDL_FreeSurface(surf);
    snprintf(e->text, sizeof e->text, "%s", text);
    e->size = size;
    e->color = color;
    e->used = g_frame;
    return e->tex ? e : NULL;
}

/* A string can mix normal text and button glyphs ("Hold <+> and <->"): it is
 * drawn in runs, each with the font that has those characters. */
static bool has_glyph(const char* s) { return strchr(s, '\xee') != NULL; }

/* Length of the run starting at s: glyphs (3-byte U+E0xx) or plain text. */
static size_t run_length(const char* s) {
    const bool glyph = (unsigned char)s[0] == 0xEE;
    size_t n = 0;
    while (s[n] && (((unsigned char)s[n] == 0xEE) == glyph)) n += glyph ? 3 : 1;
    return n;
}

static int single_width(const char* text, int size) {
    int w = 0, h = 0;
    TTF_Font* f = font_for(size, is_glyph_text(text));
    if (f) TTF_SizeUTF8(f, text, &w, &h);
    return w;
}

int ui_text_width(const char* text, int size) {
    if (!text || !text[0]) return 0;
    if (!has_glyph(text) || run_length(text) == strlen(text)) return single_width(text, size);
    int w = 0;
    char run[160];
    for (const char* p = text; *p;) {
        const size_t n = run_length(p);
        snprintf(run, sizeof run, "%.*s", (int)n, p);
        w += single_width(run, size);
        p += n;
    }
    return w;
}

int ui_text(int x, int y, const char* text, int size, SDL_Color c, int max_w, int align) {
    if (!text || !text[0]) return 0;
    if (has_glyph(text) && run_length(text) != strlen(text)) {
        /* Mixed text and glyphs: draw each run in turn (no ellipsis needed for hints). */
        const int total = ui_text_width(text, size);
        int dx = align == UI_CENTER ? x - total / 2 : align == UI_RIGHT ? x - total : x;
        char run[160];
        for (const char* p = text; *p;) {
            const size_t n = run_length(p);
            snprintf(run, sizeof run, "%.*s", (int)n, p);
            dx += ui_text(dx, y, run, size, c, 0, UI_LEFT);
            p += n;
        }
        return total;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "%s", text);
    if (max_w > 0 && ui_text_width(buf, size) > max_w) {
        /* Drop whole UTF-8 characters from the end until "text…" fits. */
        size_t n = strlen(buf);
        while (n > 0) {
            do n--;
            while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
            memcpy(buf + n, "\xe2\x80\xa6", 4);   /* U+2026 … */
            if (ui_text_width(buf, size) <= max_w) break;
        }
    }
    cache_entry_t* e = render_cached(buf, size, c);
    if (!e) return 0;
    int dx = x;
    if (align == UI_CENTER) dx = x - e->w / 2;
    else if (align == UI_RIGHT) dx = x - e->w;
    SDL_Rect dst = {dx, y, e->w, e->h};
    SDL_RenderCopy(g_ren, e->tex, NULL, &dst);
    return e->w;
}

void ui_fill(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_ren, &r);
}

void ui_round_rect(int x, int y, int w, int h, int r, SDL_Color c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    ui_fill(x + r, y, w - 2 * r, h, c);
    ui_fill(x, y + r, r, h - 2 * r, c);
    ui_fill(x + w - r, y + r, r, h - 2 * r, c);
    /* Corners: one horizontal line per pixel row of the quarter circle. */
    for (int dy = 0; dy < r; dy++) {
        const float fy = r - dy - 0.5f;
        const int dx = (int)(r - SDL_sqrtf((float)r * r - fy * fy) + 0.5f);
        ui_fill(x + dx, y + dy, r - dx, 1, c);
        ui_fill(x + w - r, y + dy, r - dx, 1, c);
        ui_fill(x + dx, y + h - 1 - dy, r - dx, 1, c);
        ui_fill(x + w - r, y + h - 1 - dy, r - dx, 1, c);
    }
}

void ui_begin(void) {
    g_frame++;
    SDL_SetRenderDrawColor(g_ren, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(g_ren);
}

bool ui_in_focus(void) { return appletGetFocusState() == AppletFocusState_InFocus; }

void ui_present(void) {
    if (ui_in_focus()) SDL_RenderPresent(g_ren);
    else svcSleepThread(16000000);   /* not visible: don't wait on vsync */
}

void ui_header(const char* subtitle) {
    ui_fill(0, 0, UI_W, 72, UI_BAR);
    /* Five-petal flower like Eglantine's logo, made of circles of rectangles. */
    const int cx = 44, cy = 36;
    static const int px[5] = {0, 12, 8, -8, -12}, py[5] = {-12, -4, 10, 10, -4};
    for (int i = 0; i < 5; i++) ui_round_rect(cx + px[i] - 9, cy + py[i] - 9, 18, 18, 9, UI_ACCENT);
    ui_round_rect(cx - 4, cy - 4, 8, 8, 4, UI_TEXT);
    ui_text(76, 16, "Eglantine Bridge", 30, UI_TEXT, 0, UI_LEFT);
    if (subtitle) ui_text(UI_W - 32, 22, subtitle, 22, UI_TEXT_DIM, 700, UI_RIGHT);
    ui_fill(0, 72, UI_W, 2, UI_ACCENT);
}

void ui_footer(const ui_hint_t* hints, int count, const char* left_text) {
    ui_fill(0, UI_H - 56, UI_W, 56, UI_BAR);
    ui_fill(0, UI_H - 56, UI_W, 1, (SDL_Color){0x3a, 0x3f, 0x45, 0xff});
    if (left_text) ui_text(32, UI_H - 42, left_text, 20, UI_TEXT_DIM, 600, UI_LEFT);
    int x = UI_W - 32;
    for (int i = count - 1; i >= 0; i--) {
        x -= ui_text(x, UI_H - 42, hints[i].label, 22, UI_TEXT, 0, UI_RIGHT) + 8;
        x -= ui_text(x, UI_H - 46, hints[i].glyph, 28, UI_TEXT, 0, UI_RIGHT) + 36;
    }
}

void ui_card(const char* title, SDL_Color title_color, const char* const* lines, int count,
             const char* hint) {
    const int w = 860, line_h = 36;
    const int h = 120 + count * line_h + (hint ? 50 : 0);
    const int x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_round_rect(x, y, w, h, 16, UI_ROW_A);
    ui_fill(x + 16, y, w - 32, 4, title_color);
    ui_text(UI_W / 2, y + 28, title, 32, title_color, w - 60, UI_CENTER);
    for (int i = 0; i < count; i++)
        ui_text(UI_W / 2, y + 90 + i * line_h, lines[i], 24, UI_TEXT, w - 60, UI_CENTER);
    if (hint) ui_text(UI_W / 2, y + h - 52, hint, 22, UI_TEXT_DIM, w - 60, UI_CENTER);
}

void ui_wait(const char* title, const char* line) {
    ui_begin();
    ui_header(NULL);
    const char* lines[1] = {line};
    ui_card(title, UI_ACCENT, lines, line ? 1 : 0, NULL);
    ui_present();
}

void ui_message(PadState* pad, const char* title, SDL_Color title_color, const char* const* lines,
                int count) {
    while (appletMainLoop()) {
        padUpdate(pad);
        int tx, ty;
        if (padGetButtonsDown(pad) || ui_tap(&tx, &ty)) break;
        ui_begin();
        ui_header(NULL);
        ui_card(title, title_color, lines, count, "Press any button to continue");
        ui_present();
    }
}

bool ui_tap(int* x, int* y) {
    static bool down;
    static int sx, sy, lx, ly;
    HidTouchScreenState st = {0};
    hidGetTouchScreenStates(&st, 1);
    if (st.count > 0) {
        lx = (int)st.touches[0].x;
        ly = (int)st.touches[0].y;
        if (!down) {
            sx = lx;
            sy = ly;
        }
        down = true;
        return false;
    }
    if (!down) return false;
    down = false;
    if (abs(lx - sx) > 30 || abs(ly - sy) > 30) return false;   /* a swipe, not a tap */
    *x = lx;
    *y = ly;
    return true;
}

/* Card in the middle of the screen: game name, what Eglantine is doing, progress. */
void ui_loading_card(const char* game, const char* text, SDL_Color accent, int value, int total,
                              double t) {
    ui_fill(0, 0, UI_W, UI_H, (SDL_Color){0x10, 0x12, 0x14, 0xc8});   /* dim the picture */
    const int w = 820, h = 250, x = (UI_W - w) / 2, y = (UI_H - h) / 2;
    ui_round_rect(x, y, w, h, 18, UI_ROW_A);
    ui_fill(x + 18, y, w - 36, 4, accent);
    ui_text(UI_W / 2, y + 26, "EGLANTINE", 18, UI_TEXT_DIM, 0, UI_CENTER);
    ui_text(UI_W / 2, y + 54, game && game[0] ? game : "Eglantine Bridge", 34, UI_TEXT, w - 60, UI_CENTER);
    ui_text(UI_W / 2, y + 108, text, 24, UI_TEXT, w - 60, UI_CENTER);

    const int bx = x + 60, by = y + 160, bw = w - 120, bh = 12;
    ui_round_rect(bx, by, bw, bh, 6, UI_ROW_B);
    if (total > 0) {
        const int fill = (int)((long long)bw * (value < total ? value : total) / total);
        if (fill > 0) ui_round_rect(bx, by, fill > bh ? fill : bh, bh, 6, accent);
        char num[48];
        snprintf(num, sizeof num, "%d / %d  (%d%%)", value, total, (int)((long long)value * 100 / total));
        ui_text(UI_W / 2, by + 24, num, 20, UI_TEXT_DIM, 0, UI_CENTER);
    } else if (accent.r != UI_RED.r || accent.g != UI_RED.g) {
        /* Indeterminate: a segment sliding back and forth. */
        const int seg = bw / 4;
        const double phase = (SDL_sin(t * 2.4) + 1.0) / 2.0;
        ui_round_rect(bx + (int)((bw - seg) * phase), by, seg, bh, 6, accent);
    }
}

