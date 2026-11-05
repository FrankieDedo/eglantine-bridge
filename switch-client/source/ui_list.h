// List screen shared by the PC list and the game list (Eglantine-style rows).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

enum { ICON_NONE, ICON_TEXTURE, ICON_PLAY, ICON_PC, ICON_ADD, ICON_LETTER };

typedef struct {
    const char* title;
    const char* subtitle;
    const char* col[3];
    int icon_kind;
    SDL_Texture* icon;
    const char* badge;   /* small pill after the title, e.g. "In corso" */
    SDL_Color badge_color;
} row_t;

#define LIST_Y 116
#define ROW_H 88
#define VISIBLE_ROWS 6

/* heads: name column title, then up to three column titles (NULL = none). */
void draw_list(const row_t* rows, int count, int sel, int top, const char* const heads[4]);
/* Moves the cursor with buttons and the touch screen. Returns true when the
 * selected row is activated (A, or a tap on the row that is already selected). */
bool list_input(uint64_t down, int count, int* sel, int* top);
