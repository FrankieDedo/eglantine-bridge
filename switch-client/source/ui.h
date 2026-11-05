// Graphical interface, in the style of Eglantine's game list: dark theme,
// alternating rows, the Switch system font and its button glyphs.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>
#include <switch.h>

#define UI_W 1280
#define UI_H 720

/* Palette (from Eglantine's dark theme). */
#define UI_BG          (SDL_Color){0x20, 0x24, 0x28, 0xff}
#define UI_BAR         (SDL_Color){0x17, 0x1a, 0x1d, 0xff}
#define UI_ROW_A       (SDL_Color){0x2b, 0x30, 0x35, 0xff}
#define UI_ROW_B       (SDL_Color){0x25, 0x29, 0x2e, 0xff}
#define UI_ROW_SEL     (SDL_Color){0x45, 0x2c, 0x38, 0xff}
#define UI_ACCENT      (SDL_Color){0xe2, 0x3a, 0x62, 0xff}   /* Eglantine pink */
#define UI_TEXT        (SDL_Color){0xf2, 0xf2, 0xf2, 0xff}
#define UI_TEXT_DIM    (SDL_Color){0xa0, 0xa6, 0xad, 0xff}
#define UI_GREEN       (SDL_Color){0x3c, 0xc0, 0x6c, 0xff}
#define UI_RED         (SDL_Color){0xe5, 0x4b, 0x4b, 0xff}
#define UI_YELLOW      (SDL_Color){0xf0, 0xc0, 0x40, 0xff}

/* Button glyphs of the system font (Nintendo extension font). */
#define UI_GLYPH_A     "\xee\x83\xa0"   /* U+E0E0 */
#define UI_GLYPH_B     "\xee\x83\xa1"
#define UI_GLYPH_X     "\xee\x83\xa2"
#define UI_GLYPH_Y     "\xee\x83\xa3"
#define UI_GLYPH_PLUS  "\xee\x83\xaf"   /* U+E0EF */
#define UI_GLYPH_MINUS "\xee\x83\xb0"
#define UI_GLYPH_DPAD  "\xee\x83\xaa"   /* U+E0EA */

enum { UI_LEFT, UI_CENTER, UI_RIGHT };

bool ui_init(void);
void ui_exit(void);
SDL_Window* ui_window(void);
SDL_Renderer* ui_renderer(void);

void ui_begin(void);     /* clears to the background colour */
void ui_present(void);   /* shows the frame (waits for vsync); skipped in the HOME menu */
/* False while the HOME menu (or another app) is in front: the app keeps running
 * in the background so the connection stays alive, but nothing is shown. */
bool ui_in_focus(void);

void ui_fill(int x, int y, int w, int h, SDL_Color c);
void ui_round_rect(int x, int y, int w, int h, int r, SDL_Color c);
/* Draws UTF-8 text; cut with an ellipsis to max_w (0 = no limit). Returns the width. */
int ui_text(int x, int y, const char* text, int size, SDL_Color c, int max_w, int align);
int ui_text_width(const char* text, int size);

/* Top bar: app name and a subtitle on the right. */
void ui_header(const char* subtitle);
/* Bottom bar with button hints, e.g. {UI_GLYPH_A, "Avvia"}, right aligned. */
typedef struct {
    const char* glyph;
    const char* label;
} ui_hint_t;
void ui_footer(const ui_hint_t* hints, int count, const char* left_text);

/* A centred card with a title and lines of text (no input handling). */
void ui_card(const char* title, SDL_Color title_color, const char* const* lines, int count,
             const char* hint);
/* Shows a card until a button or the touch screen is pressed. */
void ui_message(PadState* pad, const char* title, SDL_Color title_color, const char* const* lines,
                int count);
/* Shows a card for one frame (for "please wait" screens). */
void ui_wait(const char* title, const char* line);

/* Loading card over the stream: game name, what the emulator is doing and a
 * progress bar (value/total, or an animated one if total is 0; none if red). */
void ui_loading_card(const char* game, const char* text, SDL_Color accent, int value, int total, double t);

/* Touch: returns true once per tap (finger lifted without moving much). */
bool ui_tap(int* x, int* y);
