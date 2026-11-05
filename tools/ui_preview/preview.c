// Draws the Switch app's screens on a PC and saves them as BMP files, to check
// the layout without a console:  preview.exe <output folder>
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "ui_list.h"

static void save(const char* dir, const char* name) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, UI_W, UI_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderReadPixels(ui_renderer(), NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

static SDL_Texture* gradient_icon(Uint8 r, Uint8 g, Uint8 b) {
    static Uint8 px[64 * 64 * 3];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            Uint8* p = px + (y * 64 + x) * 3;
            p[0] = (Uint8)(r * (64 + x + y) / 192);
            p[1] = (Uint8)(g * (64 + x + y) / 192);
            p[2] = (Uint8)(b * (64 + x + y) / 192);
        }
    SDL_Texture* t = SDL_CreateTexture(ui_renderer(), SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, 64, 64);
    SDL_UpdateTexture(t, NULL, px, 64 * 3);
    return t;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!ui_init()) {
        fprintf(stderr, "ui_init: %s\n", SDL_GetError());
        return 1;
    }

    /* Game list */
    row_t games[8] = {0};
    games[0] = (row_t){"Pokopia", "Continue the game already running on the PC", {0}, ICON_PLAY, NULL, "Running", UI_GREEN};
    games[1] = (row_t){"Pokopia", "0x0100a3a01ee4c000", {"Directory", "Not scanned", "02:48:50"}, ICON_TEXTURE,
                       gradient_icon(226, 58, 98), "Running", UI_GREEN};
    games[2] = (row_t){"Example Town Adventure", "0x0100000000020000", {"XCI", "15.8 GiB", "00:12:03"},
                       ICON_TEXTURE, gradient_icon(60, 140, 230), NULL, {0}};
    games[3] = (row_t){"Yoshi and the Mysterious Book", "0x0100000000030000", {"NSP", "3.1 GiB", ""},
                       ICON_LETTER, NULL, NULL, {0}};
    games[4] = (row_t){"A very long title to see how it gets cut on the console screen",
                       NULL, {"Directory", "Not scanned", ""}, ICON_TEXTURE, gradient_icon(80, 200, 120), NULL, {0}};
    static const char* const gheads[4] = {"Name", "File type", "Size", "Play time"};
    static const ui_hint_t ghints[] = {{UI_GLYPH_A, "Start"}, {UI_GLYPH_B, "Back"}};
    ui_begin();
    ui_header("PC-FRANCESCO");
    draw_list(games, 5, 2, 0, gheads);
    ui_footer(ghints, 2, "4 games");
    save(dir, "giochi");

    /* PC list */
    row_t pcs[3] = {0};
    pcs[0] = (row_t){"Eglantine", "192.168.1.20:52141", {"On the network"}, ICON_PC, NULL, NULL, {0}};
    pcs[1] = (row_t){"PC-salotto", "192.168.1.35:52141", {"Saved"}, ICON_PC, NULL, "Busy", UI_YELLOW};
    pcs[2] = (row_t){"Enter the PC's address", "IP and port, also to play away from home", {0}, ICON_ADD,
                     NULL, NULL, {0}};
    static const char* const pheads[4] = {"PC", "Status", NULL, NULL};
    static const ui_hint_t phints[] = {{UI_GLYPH_A, "Connect"}, {UI_GLYPH_Y, "Search again"}, {UI_GLYPH_PLUS, "Exit"}};
    ui_begin();
    ui_header("Choose a PC");
    draw_list(pcs, 3, 0, 0, pheads);
    ui_footer(phints, 3, "1 PC found on the network");
    save(dir, "pc");

    /* Loading screens over a (fake) game picture */
    SDL_Texture* bg = gradient_icon(60, 140, 230);
    ui_begin();
    SDL_RenderCopy(ui_renderer(), bg, NULL, NULL);
    ui_loading_card("Pokopia", "Loading shaders", UI_ACCENT, 120, 395, 0.0);
    save(dir, "caricamento");
    ui_begin();
    SDL_RenderCopy(ui_renderer(), bg, NULL, NULL);
    ui_loading_card("Pokopia", "Starting the game", UI_ACCENT, 0, 0, 0.4);
    {
        const char* hint = "Hold " UI_GLYPH_PLUS " + " UI_GLYPH_MINUS " for 1 second to return to the menu";
        const int w = ui_text_width(hint, 20) + 40;
        ui_round_rect((UI_W - w) / 2, UI_H - 70, w, 40, 20, (SDL_Color){0x10, 0x12, 0x14, 0xd8});
        ui_text(UI_W / 2, UI_H - 62, hint, 20, UI_TEXT, 0, UI_CENTER);
    }
    save(dir, "avvio");

    /* Message */
    const char* lines[4] = {"Network latency: 10.2 ms", "Frames shown: 3392   lost: 0",
                            "Average decode: 4.1 ms per frame", "Audio: 5612 packets, 0 underruns"};
    ui_begin();
    ui_header(NULL);
    ui_card("Session ended", UI_ACCENT, lines, 4, "Press any button to continue");
    save(dir, "messaggio");

    ui_exit();
    return 0;
}
