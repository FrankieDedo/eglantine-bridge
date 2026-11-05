// Eglantine Bridge - Switch client: pick a PC and a game, then stream it
// (video, audio, controls, rumble). The menus are drawn with SDL (ui.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <pthread.h>

#include "audio.h"
#include "input.h"
#include "net.h"
#include "store.h"
#include "ui.h"
#include "ui_list.h"
#include "video.h"

#define MAX_ENTRIES 32
#define SEC_NS 1000000000ULL
/* Written once per second while streaming: lets us debug without a screen. */
#define LOG_PATH "sdmc:/config/eglantine-bridge/log.txt"

typedef struct {
    char ip[64];
    uint16_t port;
    char name[64];
    bool manual;   /* "enter IP:port" entry */
    bool found;    /* answered the LAN search (vs. only saved) */
    bool busy;     /* already streaming to another console */
} entry_t;

static store_t g_store;
static PadState g_pad;

static u64 now_ns(void) { return armTicksToNs(armGetSystemTick()); }

static bool is_application_mode(void) {
    AppletType t = appletGetAppletType();
    return t == AppletType_Application || t == AppletType_SystemApplication;
}

/* ------------------------------------------------------------------ */
/* Keyboard and PIN                                                     */
/* ------------------------------------------------------------------ */

/* Software keyboard. Only available when started as an application
 * (the NSP, or hbmenu opened by holding R on a game). */
static bool keyboard(const char* header, const char* initial, bool numeric, char* out, size_t out_len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    if (numeric) {
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
        swkbdConfigSetStringLenMax(&kbd, 4);
    }
    Result rc = swkbdShow(&kbd, out, out_len);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0];
}

/* PIN entry with the D-pad, for applet mode (no keyboard). */
static bool pin_with_dpad(uint32_t* pin) {
    int digits[4] = {0}, pos = 0;
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_AnyUp) digits[pos] = (digits[pos] + 1) % 10;
        if (down & HidNpadButton_AnyDown) digits[pos] = (digits[pos] + 9) % 10;
        if (down & HidNpadButton_AnyRight) pos = (pos + 1) % 4;
        if (down & HidNpadButton_AnyLeft) pos = (pos + 3) % 4;
        if (down & HidNpadButton_B) return false;
        if (down & HidNpadButton_A) {
            *pin = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
            return true;
        }
        ui_begin();
        ui_header("Pairing");
        const char* lines[1] = {"Enter the PIN shown on the PC"};
        ui_card("Pairing", UI_ACCENT, lines, 1, NULL);
        for (int i = 0; i < 4; i++) {
            const int x = UI_W / 2 - 190 + i * 100, y = UI_H / 2 + 10;
            ui_round_rect(x, y, 80, 96, 12, i == pos ? UI_ACCENT : UI_ROW_B);
            char d[2] = {(char)('0' + digits[i]), 0};
            ui_text(x + 40, y + 16, d, 56, UI_TEXT, 0, UI_CENTER);
        }
        static const ui_hint_t hints[] = {{UI_GLYPH_DPAD, "Change"}, {UI_GLYPH_A, "Confirm"}, {UI_GLYPH_B, "Cancel"}};
        ui_footer(hints, 3, NULL);
        ui_present();
    }
    return false;
}

static bool ask_pin(uint32_t* pin) {
    char text[8] = {0};
    if (is_application_mode()) {
        if (!keyboard("PIN shown on the PC", NULL, true, text, sizeof text)) return false;
        *pin = (uint32_t)strtoul(text, NULL, 10);
        return true;
    }
    return pin_with_dpad(pin);
}

/* Parses "192.168.1.10:52141", "[host]" or "host:port". */
static bool parse_address(const char* text, char* ip, size_t ip_len, uint16_t* port) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s", text);
    char* colon = strrchr(buf, ':');
    *port = EB_DEFAULT_PORT;
    if (colon) {
        *colon = 0;
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) return false;
        *port = (uint16_t)p;
    }
    if (!buf[0]) return false;
    snprintf(ip, ip_len, "%s", buf);
    return true;
}

static int build_entries(entry_t* e, const eb_found_host_t* found, int found_count) {
    int n = 0;
    for (int i = 0; i < found_count && n < MAX_ENTRIES - 1; i++, n++) {
        memset(&e[n], 0, sizeof e[n]);
        snprintf(e[n].ip, sizeof e[n].ip, "%s", found[i].ip);
        snprintf(e[n].name, sizeof e[n].name, "%s", found[i].name);
        e[n].port = found[i].port;
        e[n].found = true;
        e[n].busy = (found[i].flags & EB_HOST_FLAG_BUSY) != 0;
    }
    for (int i = 0; i < g_store.host_count && n < MAX_ENTRIES - 1; i++) {
        const saved_host_t* h = &g_store.hosts[i];
        bool dup = false;
        for (int j = 0; j < found_count; j++)
            if (!strcmp(found[j].ip, h->ip) && found[j].port == h->port) dup = true;
        if (dup) continue;
        memset(&e[n], 0, sizeof e[n]);
        snprintf(e[n].ip, sizeof e[n].ip, "%s", h->ip);
        snprintf(e[n].name, sizeof e[n].name, "%s", h->name[0] ? h->name : "PC");
        e[n].port = h->port;
        n++;
    }
    memset(&e[n], 0, sizeof e[n]);
    e[n].manual = true;
    return n + 1;
}

/* ------------------------------------------------------------------ */
/* Streaming                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    eb_session_t* s;
    video_t* video;
    audio_t* audio;            /* NULL if audout couldn't start: play without sound */
    bool stop;                 /* atomic: any thread can end the session */
    bool lost;                 /* connection lost (vs. user quit) */
    bool background;           /* atomic: HOME menu in front, keep the session but play nothing */
    pthread_mutex_t rumble_lock;
    eb_rumble_t rumble;
    bool rumble_pending;
    uint32_t udp_packets, rumble_packets;   /* atomic counters, for the log */
} stream_ctx_t;

static void* net_main(void* arg) {
    stream_ctx_t* c = arg;
    uint8_t buf[EB_MAX_UDP_PACKET + 64];
    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED)) {
        const int n = eb_recv_udp(c->s, buf, sizeof buf, 50);
        if (n <= 0) continue;
        __atomic_fetch_add(&c->udp_packets, 1, __ATOMIC_RELAXED);
        eb_udp_header_t h;
        memcpy(&h, buf, sizeof h);
        const uint8_t* payload = buf + sizeof h;
        const size_t len = (size_t)n - sizeof h;
        if (h.type == EB_UDP_VIDEO && c->video) {
            video_push_fragment(c->video, payload, len);
        } else if (h.type == EB_UDP_AUDIO && c->audio && !__atomic_load_n(&c->background, __ATOMIC_RELAXED)) {
            audio_push_packet(c->audio, payload, len);
        } else if (h.type == EB_UDP_RUMBLE && len >= sizeof(eb_rumble_t)) {
            __atomic_fetch_add(&c->rumble_packets, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&c->rumble_lock);
            memcpy(&c->rumble, payload, sizeof c->rumble);
            c->rumble_pending = true;
            pthread_mutex_unlock(&c->rumble_lock);
        }
    }
    return NULL;
}

static void write_log(stream_ctx_t* c, FILE* log, u64 start) {
    if (!log) return;
    video_stats_t vs = {0};
    if (c->video) video_get_stats(c->video, &vs);
    fprintf(log,
            "t=%.1fs udp=%u rumble=%u fragments=%u frames_received=%u lost=%u decoded=%u "
            "decode_errors=%u shown=%u waiting_keyframe=%d rtt=%.1fms\n",
            (now_ns() - start) / 1e9, __atomic_load_n(&c->udp_packets, __ATOMIC_RELAXED),
            __atomic_load_n(&c->rumble_packets, __ATOMIC_RELAXED), vs.fragments, vs.frames_received,
            vs.frames_lost, vs.pictures, vs.decode_errors, vs.frames_shown, vs.need_key,
            c->s->last_rtt_us / 1000.0);
    if (c->video) fprintf(log, "  %s\n", video_info(c->video));
    if (c->audio) {
        audio_stats_t as;
        audio_get_stats(c->audio, &as);
        fprintf(log, "  audio: packets=%u concealed=%u underruns=%u dropped=%ums delay=%ums\n", as.packets,
                as.concealed, as.underruns, as.dropped_ms, as.buffered_ms);
    }
    fflush(log);
}

static void* input_main(void* arg) {
    stream_ctx_t* c = arg;
    u64 last_ping = 0, last_idr = 0, exit_since = 0, last_log = 0;
    const u64 start = now_ns();
    FILE* log = fopen(LOG_PATH, "w");
    if (log) fprintf(log, "video: %s  audio: %s\n", c->video ? "ok" : video_last_error(),
                     c->audio ? "ok" : "not available");
    while (!__atomic_load_n(&c->stop, __ATOMIC_RELAXED)) {
        padUpdate(&g_pad);
        eb_input_t in;
        input_read(&g_pad, &in);
        const u64 t = now_ns();

        /* Hold + and - for one second to leave. */
        const u64 held = padGetButtons(&g_pad);
        if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus)) {
            if (!exit_since) exit_since = t;
            if (t - exit_since > SEC_NS) break;
            in.buttons &= ~(EB_BTN_PLUS | EB_BTN_MINUS);
        } else {
            exit_since = 0;
        }
        eb_send_input(c->s, &in);

        pthread_mutex_lock(&c->rumble_lock);
        const bool rumble = c->rumble_pending;
        const eb_rumble_t r = c->rumble;
        c->rumble_pending = false;
        pthread_mutex_unlock(&c->rumble_lock);
        const bool background = __atomic_load_n(&c->background, __ATOMIC_RELAXED);
        if (rumble && !background) input_rumble(&g_pad, &r);

        if (!background && c->video && video_needs_keyframe(c->video) && t - last_idr > SEC_NS / 4) {
            eb_request_idr(c->s);
            last_idr = t;
        }
        if (t - last_ping > SEC_NS) {
            eb_send_ping(c->s);
            last_ping = t;
        }
        if (t - last_log > SEC_NS) {
            write_log(c, log, start);
            last_log = t;
        }
        if (!eb_poll_control(c->s)) {
            c->lost = true;
            break;
        }
        svcSleepThread(8000000);   /* ~120 input packets per second */
    }
    write_log(c, log, start);
    if (log) {
        fprintf(log, "fine: %s\n", c->lost ? "connection lost" : "quit");
        fclose(log);
    }
    __atomic_store_n(&c->stop, true, __ATOMIC_RELAXED);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Game menu                                                            */
/* ------------------------------------------------------------------ */

#define MAX_GAMES 256
static eb_game_t g_games[MAX_GAMES];
/* Name of the game just started from the menu (empty if none): the loading screen
 * shows it until the PC reports that it is booting. */
static char g_launch_name[EB_GAME_NAME_LEN];

/* Lets the user pick the game the PC should start. Returns false to go back
 * to the PC list (B, or connection lost). */
static bool game_menu(eb_session_t* s, const char* pc_name) {
    ui_wait("Getting the game list...", pc_name);
    const int n = eb_list_games(s, g_games, MAX_GAMES, 1500);
    if (n < 0) return true;   /* older PC side without a game list: just stream */

    int running = -1;
    for (int i = 0; i < n; i++)
        if (g_games[i].running) running = i;

    /* Row 0 = stream without starting anything; rows 1..n = games. */
    static row_t rows[MAX_GAMES + 1];
    static SDL_Texture* icons[MAX_GAMES];
    static char ids[MAX_GAMES][24];
    rows[0] = (row_t){0};
    rows[0].icon_kind = ICON_PLAY;
    if (running >= 0) {
        rows[0].title = g_games[running].name;
        rows[0].subtitle = "Continue the game already running on the PC";
        rows[0].badge = "Running";
        rows[0].badge_color = UI_GREEN;
    } else {
        rows[0].title = "Go to the stream";
        rows[0].subtitle = "Without starting a game: shows what is on the PC";
    }
    for (int i = 0; i < n; i++) {
        row_t* r = &rows[i + 1];
        *r = (row_t){0};
        r->title = g_games[i].name;
        if (g_games[i].title_id) {
            snprintf(ids[i], sizeof ids[i], "0x%016llx", (unsigned long long)g_games[i].title_id);
            r->subtitle = ids[i];
        }
        r->col[0] = g_games[i].file_type;
        r->col[1] = g_games[i].size;
        r->col[2] = g_games[i].play_time;
        icons[i] = NULL;
        if (g_games[i].icon_bytes == EB_ICON_BYTES) {
            icons[i] = SDL_CreateTexture(ui_renderer(), SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC,
                                         EB_ICON_SIZE, EB_ICON_SIZE);
            if (icons[i]) SDL_UpdateTexture(icons[i], NULL, g_games[i].icon, EB_ICON_SIZE * 3);
        }
        r->icon = icons[i];
        r->icon_kind = icons[i] ? ICON_TEXTURE : ICON_LETTER;
        if (g_games[i].running) {
            r->badge = "Running";
            r->badge_color = UI_GREEN;
        }
    }
    const int total = n + 1;
    int sel = (running < 0 && n > 0) ? 1 : 0, top = 0;
    u64 last_keepalive = 0;
    bool result = false;
    char count_text[48];
    snprintf(count_text, sizeof count_text, n == 1 ? "1 game" : "%d games", n);
    static const char* const heads[4] = {"Name", "File type", "Size", "Play time"};
    static const ui_hint_t hints[] = {{UI_GLYPH_A, "Start"}, {UI_GLYPH_B, "Back"}};

    while (appletMainLoop()) {
        padUpdate(&g_pad);
        const u64 down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_B) break;
        if (list_input(down, total, &sel, &top)) {
            if (sel > 0 && !g_games[sel - 1].running) {
                eb_launch_game(s, g_games[sel - 1].index);
                snprintf(g_launch_name, sizeof g_launch_name, "%s", g_games[sel - 1].name);
                ui_wait("Starting the game on the PC...", g_games[sel - 1].name);
            }
            result = true;
            break;
        }

        /* The stream hasn't started yet: keep the session alive by hand. */
        const u64 t = now_ns();
        if (t - last_keepalive > SEC_NS) {
            eb_send_ping(s);
            eb_send_heartbeat(s);
            last_keepalive = t;
        }
        if (!eb_poll_control(s)) {
            const char* lines[1] = {"The PC stopped answering."};
            ui_message(&g_pad, "Connection lost", UI_RED, lines, 1);
            break;
        }

        ui_begin();
        ui_header(pc_name);
        draw_list(rows, total, sel, top, heads);
        if (n == 0) ui_text(UI_W / 2, LIST_Y + ROW_H + 40, "Eglantine's game library is empty.", 24,
                            UI_TEXT_DIM, 0, UI_CENTER);
        ui_footer(hints, 2, count_text);
        ui_present();
    }
    for (int i = 0; i < n; i++)
        if (icons[i]) SDL_DestroyTexture(icons[i]);
    return result;
}

/* ------------------------------------------------------------------ */
/* Loading screen, drawn over the stream                                */
/* ------------------------------------------------------------------ */

typedef struct {
    eb_session_t* s;
    u64 start_ns;
    bool launch_pending;   /* a game was started from the menu, the PC hasn't said "booting" yet */
} overlay_ctx_t;

static bool draw_overlay(void* user, bool has_picture) {
    overlay_ctx_t* o = user;
    const u64 now = now_ns();
    const double t = (now - o->start_ns) / 1e9;
    eb_ctl_status_t st;
    const bool have = eb_get_status(o->s, &st);
    if (have && (st.state == EB_STATUS_BOOTING || st.state == EB_STATUS_LOADING || st.state == EB_STATUS_ERROR))
        o->launch_pending = false;
    if (o->launch_pending && t > 20) o->launch_pending = false;   /* never heard back: stop waiting */

    bool animating = false;
    if (o->launch_pending) {
        ui_loading_card(g_launch_name, "Starting the game", UI_ACCENT, 0, 0, t);
        animating = true;
    } else if (have && (st.state == EB_STATUS_BOOTING || st.state == EB_STATUS_LOADING)) {
        ui_loading_card(st.game, st.text, UI_ACCENT, (int)st.value, (int)st.total, t);
        animating = true;
    } else if (have && st.state == EB_STATUS_ERROR) {
        ui_loading_card(st.game, st.text, UI_RED, 0, 0, t);
    } else if (have && st.state == EB_STATUS_IDLE) {
        ui_loading_card(NULL, st.text[0] ? st.text : "No game is running on the PC", UI_TEXT_DIM, 0, 0, t);
        ui_text(UI_W / 2, UI_H / 2 + 150, "Hold " UI_GLYPH_PLUS " and " UI_GLYPH_MINUS " to go back and pick a game", 22,
                UI_TEXT_DIM, 0, UI_CENTER);
    } else if (!has_picture) {
        ui_loading_card(NULL, "Waiting for the video", UI_ACCENT, 0, 0, t);
        animating = true;
    }

    /* For the first seconds: how to leave the stream. */
    if (t < 6) {
        const char* hint = "Hold " UI_GLYPH_PLUS " + " UI_GLYPH_MINUS " for 1 second to return to the menu";
        const int w = ui_text_width(hint, 20) + 40;
        ui_round_rect((UI_W - w) / 2, UI_H - 70, w, 40, 20, (SDL_Color){0x10, 0x12, 0x14, 0xd8});
        ui_text(UI_W / 2, UI_H - 62, hint, 20, UI_TEXT, 0, UI_CENTER);
        animating = true;
    }
    return animating;
}

static void stream(const char* ip, uint16_t port, const char* name) {
    saved_host_t* saved = store_get_host(&g_store, ip, port);
    if (name && name[0]) snprintf(saved->name, sizeof saved->name, "%s", name);
    char pc_name[96], addr[96];
    snprintf(pc_name, sizeof pc_name, "%s", saved->name[0] ? saved->name : "PC");
    snprintf(addr, sizeof addr, "%s:%u", ip, port);

    ui_wait(pc_name, "Connecting...");

    eb_session_t s;
    bool paired_now = false;
    char err[128] = {0};
    if (!eb_connect(&s, ip, port, g_store.client_id, saved->secret, &paired_now, NULL, ask_pin, err,
                    sizeof err)) {
        const char* lines[2] = {addr, err};
        ui_message(&g_pad, "Could not connect", UI_RED, lines, 2);
        return;
    }
    store_save(&g_store);   /* remembers the host, and the new secret if we just paired */

    g_launch_name[0] = 0;
    if (!game_menu(&s, pc_name)) {
        eb_close(&s, true);
        return;
    }

    /* Threads while streaming:
     *   net   : receives UDP (video fragments, audio, rumble); decodes audio
     *   audio : feeds audout (inside audio.c)
     *   input : reads the controller and sends it every ~8 ms, pings, asks for keyframes
     *   main  : decodes and shows the video (waits for vsync)                         */
    stream_ctx_t ctx = {.s = &s};
    pthread_mutex_init(&ctx.rumble_lock, NULL);

    ctx.video = video_create(ui_window(), ui_renderer());
    overlay_ctx_t overlay = {.s = &s, .start_ns = now_ns(), .launch_pending = g_launch_name[0] != 0};
    if (ctx.video) video_set_overlay(ctx.video, draw_overlay, &overlay);
    else ui_wait("Video not available", video_last_error());
    ctx.audio = audio_create();
    appletSetMediaPlaybackState(true);   /* keep the screen on while playing */
    pthread_t net_thread, input_thread;
    pthread_create(&net_thread, NULL, net_main, &ctx);
    pthread_create(&input_thread, NULL, input_main, &ctx);

    const eb_rumble_t no_rumble = {0, {0}, 0, 160, 0, 320};
    while (!__atomic_load_n(&ctx.stop, __ATOMIC_RELAXED) && appletMainLoop()) {
        const bool background = !ui_in_focus();
        if (background != __atomic_load_n(&ctx.background, __ATOMIC_RELAXED)) {
            __atomic_store_n(&ctx.background, background, __ATOMIC_RELAXED);
            if (background) input_rumble(&g_pad, &no_rumble);
        }
        if (background) {
            /* The input thread keeps sending (neutral) input and pings, so the PC
             * keeps the session; the picture resumes with a keyframe on return. */
            if (ctx.video) video_discard(ctx.video);
            svcSleepThread(20000000);
        } else if (ctx.video) {
            video_render(ctx.video, 20);
        } else {
            svcSleepThread(20000000);
        }
    }
    __atomic_store_n(&ctx.stop, true, __ATOMIC_RELAXED);
    pthread_join(input_thread, NULL);
    pthread_join(net_thread, NULL);

    audio_stats_t as = {0};
    if (ctx.audio) {
        audio_get_stats(ctx.audio, &as);
        audio_destroy(ctx.audio);
    }
    video_stats_t vs = {0};
    if (ctx.video) {
        video_get_stats(ctx.video, &vs);
        video_destroy(ctx.video);
    }
    input_rumble(&g_pad, &no_rumble);
    appletSetMediaPlaybackState(false);
    eb_close(&s, !ctx.lost);
    pthread_mutex_destroy(&ctx.rumble_lock);

    char l1[96], l2[96], l3[96], l4[96];
    snprintf(l1, sizeof l1, "Network latency: %.1f ms", s.last_rtt_us / 1000.0);
    snprintf(l2, sizeof l2, "Frames shown: %u   lost: %u", vs.frames_shown, vs.frames_lost);
    snprintf(l3, sizeof l3, "Average decode: %.1f ms per frame", vs.decode_ms);
    if (ctx.audio)
        snprintf(l4, sizeof l4, "Audio: %u packets, %u dropouts", as.packets, as.underruns);
    else
        snprintf(l4, sizeof l4, "Audio: not available");
    const char* lines[4] = {l1, l2, l3, l4};
    ui_message(&g_pad, ctx.lost ? "Connection lost" : "Session ended", ctx.lost ? UI_RED : UI_ACCENT,
               lines, 4);
}

/* ------------------------------------------------------------------ */
/* PC list                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    /* Bigger UDP receive buffer than the default 42 KB: keyframes arrive in bursts. */
    SocketInitConfig sock_cfg = *socketGetDefaultInitConfig();
    sock_cfg.udp_rx_buf_size = 0x100000;
    socketInitialize(&sock_cfg);
    nifmInitialize(NifmServiceType_User);
    input_init();
    store_load(&g_store);
    /* Keep running when the HOME button is pressed, so the PC doesn't drop the
     * connection (only possible as an application: the NSP or hbmenu over a game). */
    if (is_application_mode()) appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
    if (!ui_init()) {
        /* Without SDL there is no way to show anything: leave cleanly. */
        socketExit();
        return 1;
    }

    eb_found_host_t found[16];
    int found_count = 0;
    entry_t entries[MAX_ENTRIES];
    row_t rows[MAX_ENTRIES];
    char subtitles[MAX_ENTRIES][96];
    int entry_count = 0, sel = 0, top = 0;
    bool rescan = true;
    static const char* const heads[4] = {"PC", "Status", NULL, NULL};
    static const ui_hint_t hints[] = {{UI_GLYPH_A, "Connect"}, {UI_GLYPH_Y, "Search again"}, {UI_GLYPH_PLUS, "Exit"}};

    while (appletMainLoop()) {
        if (rescan) {
            ui_wait("Looking for Eglantine on the local network...", NULL);
            found_count = eb_discover(found, 16, 1000);
            entry_count = build_entries(entries, found, found_count);
            if (sel >= entry_count) sel = 0;
            for (int i = 0; i < entry_count; i++) {
                row_t* r = &rows[i];
                *r = (row_t){0};
                if (entries[i].manual) {
                    r->title = "Enter the PC's address";
                    r->subtitle = "IP and port, also to play away from home";
                    r->icon_kind = ICON_ADD;
                    continue;
                }
                snprintf(subtitles[i], sizeof subtitles[i], "%s:%u", entries[i].ip, entries[i].port);
                r->title = entries[i].name;
                r->subtitle = subtitles[i];
                r->icon_kind = ICON_PC;
                r->col[0] = entries[i].found ? "On the network" : "Saved";
                if (entries[i].busy) {
                    r->badge = "Busy";
                    r->badge_color = UI_YELLOW;
                }
            }
            rescan = false;
        }

        padUpdate(&g_pad);
        const u64 down = padGetButtonsDown(&g_pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_Y) {
            rescan = true;
            continue;
        }
        if (list_input(down, entry_count, &sel, &top)) {
            entry_t* e = &entries[sel];
            if (e->manual) {
                char text[96] = {0}, ip[64];
                uint16_t port;
                if (!is_application_mode()) {
                    const char* lines[2] = {"The keyboard is not available in applet mode.",
                                            "Start the app from its HOME icon, or open hbmenu holding R on a game."};
                    ui_message(&g_pad, "Keyboard not available", UI_YELLOW, lines, 2);
                } else if (keyboard("PC address (IP:port)",
                                    g_store.host_count ? g_store.hosts[0].ip : "192.168.1.", false, text,
                                    sizeof text) &&
                           parse_address(text, ip, sizeof ip, &port)) {
                    stream(ip, port, NULL);
                }
            } else {
                stream(e->ip, e->port, e->found ? e->name : NULL);
            }
            rescan = true;
            continue;
        }

        char left[64];
        snprintf(left, sizeof left, found_count == 1 ? "1 PC found on the network" : "%d PCs found on the network",
                 found_count);
        ui_begin();
        ui_header("Choose a PC");
        draw_list(rows, entry_count, sel, top, heads);
        ui_footer(hints, 3, left);
        ui_present();
    }

    ui_exit();
    input_exit();
    nifmExit();
    socketExit();
    return 0;
}
