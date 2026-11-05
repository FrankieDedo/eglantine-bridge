// Video: rebuilds frames from UDP fragments, decodes H.264, shows it with SDL2.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct video video_t;

typedef struct {
    uint32_t frames_received;   /* complete frames rebuilt from the network */
    uint32_t frames_lost;       /* incomplete or dropped */
    uint32_t frames_shown;
    uint32_t fragments;         /* video packets received */
    uint32_t decode_errors;     /* packets the decoder refused */
    uint32_t pictures;          /* pictures the decoder produced */
    bool     need_key;          /* waiting for a keyframe right now */
    float    decode_ms;         /* average decode time */
} video_stats_t;

#include <SDL2/SDL.h>

/* Creates the decoder; draws with the app's window and renderer (ui.c). */
video_t* video_create(SDL_Window* win, SDL_Renderer* ren);

/* Why the last video_create() failed (SDL or FFmpeg message). */
const char* video_last_error(void);
void video_destroy(video_t* v);

/* Network thread: one EB_UDP_VIDEO payload (eb_video_frag_t + data).
 * Returns true when the picture is broken and a keyframe should be requested. */
bool video_push_fragment(video_t* v, const uint8_t* payload, size_t len);

/* Main thread: decodes everything queued and shows the newest picture.
 * Waits up to timeout_ms for a frame. */
void video_render(video_t* v, int timeout_ms);
/* Drops the frames waiting to be shown and waits for a keyframe (used while
 * the app is in the background). */
void video_discard(video_t* v);

/* True while waiting for a keyframe after a loss (keep asking for one). */
bool video_needs_keyframe(video_t* v);

void video_get_stats(video_t* v, video_stats_t* out);

/* Called after each frame is drawn and before it is shown, to draw on top of it
 * (has_picture is false before the first frame). Return true while animating:
 * the last frame is then redrawn ~30 times a second even if no new one arrives. */
typedef bool (*video_overlay_fn)(void* user, bool has_picture);
void video_set_overlay(video_t* v, video_overlay_fn fn, void* user);

/* Renderer name, texture mode in use and any SDL error: one line for the log. */
const char* video_info(video_t* v);
