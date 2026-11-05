#include "video.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <switch.h>

#include "ebridge_protocol.h"

/* Complete frames waiting for the decoder. H.264 P-frames depend on the
 * previous ones, so frames are never skipped: if the decoder falls behind,
 * the queue is flushed and we wait for a keyframe. */
#define QUEUE_LEN 6

typedef struct {
    uint8_t* data;   /* padded with AV_INPUT_BUFFER_PADDING_SIZE zero bytes */
    size_t   size;
} frame_buf_t;

struct video {
    /* Reassembly (network thread only) */
    uint32_t cur_id;
    bool     cur_valid;
    uint32_t last_done;       /* newest frame id already completed or given up */
    bool     have_done;
    uint8_t* asm_buf;
    size_t   asm_cap, asm_size;
    uint16_t frag_count, frags_got;
    uint8_t  got[EB_VIDEO_MAX_FRAGS];
    uint8_t  cur_key;

    /* Queue (shared) */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    frame_buf_t queue[QUEUE_LEN];
    int  q_head, q_count;
    bool need_key;            /* drop non-keyframes until a keyframe arrives */

    /* Decode + display (main thread only) */
    AVCodecContext* dec;
    AVPacket*  pkt;
    AVFrame*   pic;       /* newest decoded picture (what gets shown) */
    AVFrame*   tmp;       /* receive target: FFmpeg empties it when there is no frame */
    SDL_Window*   win;
    SDL_Renderer* ren;
    SDL_Texture*  tex;
    int tex_w, tex_h;
    int tex_mode;             /* how pictures reach the screen, see show_picture() */
    struct SwsContext* sws;   /* only for TEX_RGB */
    uint8_t* rgb;
    char info[200];           /* renderer + chosen mode + last SDL error, for the log */

    /* Drawn over every frame (loading screen); asks for redraws while animating. */
    video_overlay_fn overlay;
    void* overlay_user;
    bool overlay_animating;
    double last_present_ms;

    video_stats_t stats;
    double decode_total_ms;
};

static double now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1e6; }

static char g_error[160];
const char* video_last_error(void) { return g_error; }

enum { TEX_NONE, TEX_IYUV, TEX_YV12, TEX_RGB };
static const char* const kModeNames[] = {"none", "IYUV", "YV12", "RGB (CPU conversion)"};

video_t* video_create(SDL_Window* win, SDL_Renderer* ren) {
    video_t* v = calloc(1, sizeof *v);
    if (!v) return NULL;
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->cond, NULL);
    v->need_key = true;

    g_error[0] = 0;
    v->win = win;   /* owned by ui.c */
    v->ren = ren;
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);   /* matches the PC encoder */
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(v->ren, &ri) == 0)
        snprintf(v->info, sizeof v->info, "renderer=%s formats=%u max=%dx%d", ri.name,
                 ri.num_texture_formats, ri.max_texture_width, ri.max_texture_height);
    SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
    SDL_RenderClear(v->ren);
    SDL_RenderPresent(v->ren);

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        snprintf(g_error, sizeof g_error, "FFmpeg: no H.264 decoder");
        goto fail;
    }
    v->dec = avcodec_alloc_context3(codec);
    v->dec->thread_count = 4;
    v->dec->thread_type = FF_THREAD_SLICE;   /* PC encodes 4 slices: no extra frame of delay */
    v->dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    v->dec->flags2 |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(v->dec, codec, NULL) < 0) {
        snprintf(g_error, sizeof g_error, "FFmpeg: could not open the H.264 decoder");
        goto fail;
    }
    v->pkt = av_packet_alloc();
    v->pic = av_frame_alloc();
    v->tmp = av_frame_alloc();
    return v;

fail:
    video_destroy(v);
    return NULL;
}

void video_destroy(video_t* v) {
    if (!v) return;
    if (v->dec) avcodec_free_context(&v->dec);
    if (v->pkt) av_packet_free(&v->pkt);
    if (v->pic) av_frame_free(&v->pic);
    if (v->tmp) av_frame_free(&v->tmp);
    if (v->tex) SDL_DestroyTexture(v->tex);
    if (v->sws) sws_freeContext(v->sws);
    free(v->rgb);
    for (int i = 0; i < QUEUE_LEN; i++) free(v->queue[i].data);
    free(v->asm_buf);
    pthread_mutex_destroy(&v->lock);
    pthread_cond_destroy(&v->cond);
    free(v);
}

/* Called with the lock held. */
static void flush_queue_locked(video_t* v) {
    for (int i = 0; i < QUEUE_LEN; i++) {
        free(v->queue[i].data);
        v->queue[i].data = NULL;
    }
    v->q_head = v->q_count = 0;
}

static void mark_lost(video_t* v) {
    pthread_mutex_lock(&v->lock);
    v->stats.frames_lost++;
    v->need_key = true;
    pthread_mutex_unlock(&v->lock);
}

static void finish_frame(video_t* v) {
    pthread_mutex_lock(&v->lock);
    v->stats.frames_received++;
    if (v->need_key && !v->cur_key) {
        v->stats.frames_lost++;           /* can't decode it without its reference */
    } else if (v->q_count == QUEUE_LEN) {
        flush_queue_locked(v);            /* decoder too slow: restart from a keyframe */
        v->need_key = true;
        v->stats.frames_lost++;
    } else {
        v->need_key = false;
        frame_buf_t* slot = &v->queue[(v->q_head + v->q_count) % QUEUE_LEN];
        slot->data = malloc(v->asm_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (slot->data) {
            memcpy(slot->data, v->asm_buf, v->asm_size);
            memset(slot->data + v->asm_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            slot->size = v->asm_size;
            v->q_count++;
            pthread_cond_signal(&v->cond);
        }
    }
    pthread_mutex_unlock(&v->lock);
    v->cur_valid = false;
    v->last_done = v->cur_id;
    v->have_done = true;
}

bool video_push_fragment(video_t* v, const uint8_t* payload, size_t len) {
    if (len < sizeof(eb_video_frag_t)) return false;
    eb_video_frag_t f;
    memcpy(&f, payload, sizeof f);
    const uint8_t* data = payload + sizeof f;
    const size_t data_len = len - sizeof f;
    if (f.frag_count == 0 || f.frag_count > EB_VIDEO_MAX_FRAGS || f.frag_index >= f.frag_count ||
        f.frame_size > (size_t)f.frag_count * EB_VIDEO_FRAG_DATA)
        return false;

    __atomic_fetch_add(&v->stats.fragments, 1, __ATOMIC_RELAXED);
    bool lost = false;
    if (v->have_done && (int32_t)(f.frame_id - v->last_done) <= 0) return false;   /* late or duplicate */
    if (!v->cur_valid || f.frame_id != v->cur_id) {
        if (v->cur_valid) {
            if ((int32_t)(f.frame_id - v->cur_id) < 0) return false;   /* late packet */
            mark_lost(v);            /* previous frame never completed */
            v->last_done = v->cur_id;
            v->have_done = true;
            lost = true;
        }
        v->cur_id = f.frame_id;
        v->cur_valid = true;
        v->cur_key = f.frame_flags & EB_FRAME_KEY;
        v->frag_count = f.frag_count;
        v->frags_got = 0;
        v->asm_size = f.frame_size;
        memset(v->got, 0, f.frag_count);
        if (v->asm_cap < f.frame_size) {
            free(v->asm_buf);
            v->asm_cap = f.frame_size + 64 * 1024;
            v->asm_buf = malloc(v->asm_cap);
            if (!v->asm_buf) {
                v->asm_cap = 0;
                v->cur_valid = false;
                return true;
            }
        }
    }
    if (v->got[f.frag_index]) return lost;
    const size_t offset = (size_t)f.frag_index * EB_VIDEO_FRAG_DATA;
    if (offset + data_len > v->asm_size) return lost;
    memcpy(v->asm_buf + offset, data, data_len);
    v->got[f.frag_index] = 1;
    if (++v->frags_got == v->frag_count) finish_frame(v);

    pthread_mutex_lock(&v->lock);
    lost = lost || v->need_key;
    pthread_mutex_unlock(&v->lock);
    return lost;
}

bool video_needs_keyframe(video_t* v) {
    pthread_mutex_lock(&v->lock);
    bool r = v->need_key;
    pthread_mutex_unlock(&v->lock);
    return r;
}

/* Creates the texture for the decoded size. Tries, in order: IYUV (the decoder's
 * own layout), YV12 (same planes, U/V swapped), and as a last resort an RGB texture
 * filled by libswscale on the CPU. Records what worked (or SDL's error) in v->info. */
static void create_texture(video_t* v, int w, int h) {
    if (v->tex) SDL_DestroyTexture(v->tex);
    v->tex = NULL;
    v->tex_w = w;
    v->tex_h = h;
    char errors[120] = {0};
    static const Uint32 fmts[] = {SDL_PIXELFORMAT_IYUV, SDL_PIXELFORMAT_YV12, SDL_PIXELFORMAT_ABGR8888};
    for (int i = 0; i < 3 && !v->tex; i++) {
        v->tex = SDL_CreateTexture(v->ren, fmts[i], SDL_TEXTUREACCESS_STREAMING, w, h);
        if (v->tex) {
            v->tex_mode = TEX_IYUV + i;
        } else {
            size_t n = strlen(errors);
            snprintf(errors + n, sizeof errors - n, "[%s] ", SDL_GetError());
        }
    }
    if (!v->tex) v->tex_mode = TEX_NONE;
    if (v->tex_mode == TEX_RGB && !v->rgb) v->rgb = malloc((size_t)w * h * 4);
    const size_t n = strlen(v->info);
    snprintf(v->info + n, sizeof v->info - n, " texture=%dx%d %s pixfmt=%s %s", w, h,
             kModeNames[v->tex_mode], av_get_pix_fmt_name(v->pic->format), errors);
}

static void show_picture(video_t* v) {
    AVFrame* p = v->pic;
    if (!v->tex || v->tex_w != p->width || v->tex_h != p->height) create_texture(v, p->width, p->height);
    if (!v->tex) return;
    int rc = 0;
    if (v->tex_mode == TEX_IYUV || v->tex_mode == TEX_YV12) {
        /* Takes Y, U, V and stores them in the texture's own plane order. */
        rc = SDL_UpdateYUVTexture(v->tex, NULL, p->data[0], p->linesize[0], p->data[1], p->linesize[1],
                                  p->data[2], p->linesize[2]);
    } else {
        v->sws = sws_getCachedContext(v->sws, p->width, p->height, p->format, p->width, p->height,
                                      AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!v->sws || !v->rgb) return;
        uint8_t* dst[1] = {v->rgb};
        int dst_stride[1] = {p->width * 4};
        sws_scale(v->sws, (const uint8_t* const*)p->data, p->linesize, 0, p->height, dst, dst_stride);
        rc = SDL_UpdateTexture(v->tex, NULL, v->rgb, p->width * 4);
    }
    if (rc != 0 && v->stats.frames_shown == 0) {
        const size_t n = strlen(v->info);
        snprintf(v->info + n, sizeof v->info - n, " update:[%s]", SDL_GetError());
    }
    SDL_RenderClear(v->ren);
    if (SDL_RenderCopy(v->ren, v->tex, NULL, NULL) != 0 && v->stats.frames_shown == 0) {
        const size_t n = strlen(v->info);
        snprintf(v->info + n, sizeof v->info - n, " copy:[%s]", SDL_GetError());
    }
    if (v->overlay) v->overlay_animating = v->overlay(v->overlay_user, true);
    SDL_RenderPresent(v->ren);   /* waits for vsync */
    v->last_present_ms = now_ms();
    v->stats.frames_shown++;
}

void video_set_overlay(video_t* v, video_overlay_fn fn, void* user) {
    v->overlay = fn;
    v->overlay_user = user;
}

/* Shown until the first picture: the overlay (loading screen) on the app's
 * background, and a thin diagnostic strip along the bottom edge:
 *   red    = no video packets yet
 *   yellow = packets arrive but no frame could be rebuilt/decoded
 *   green  = frames decoded, picture coming                        */
static void show_waiting(video_t* v) {
    const double t = now_ms();
    if (t - v->last_present_ms < 33) return;   /* ~30 updates per second, for the animation */
    const uint32_t frags = __atomic_load_n(&v->stats.fragments, __ATOMIC_RELAXED);
    SDL_SetRenderDrawColor(v->ren, 0x20, 0x24, 0x28, 255);
    SDL_RenderClear(v->ren);
    if (v->overlay) v->overlay(v->overlay_user, false);
    SDL_Rect bar = {0, 716, 1280, 4};
    if (v->stats.pictures > 0) SDL_SetRenderDrawColor(v->ren, 40, 200, 60, 255);
    else if (frags > 0) SDL_SetRenderDrawColor(v->ren, 230, 200, 30, 255);
    else SDL_SetRenderDrawColor(v->ren, 220, 40, 40, 255);
    SDL_RenderFillRect(v->ren, &bar);
    SDL_RenderPresent(v->ren);
    v->last_present_ms = now_ms();
}

/* No new frame, but the overlay is animating: show the last picture again. */
static void redraw(video_t* v) {
    if (!v->tex || now_ms() - v->last_present_ms < 33) return;
    SDL_RenderClear(v->ren);
    SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    v->overlay_animating = v->overlay(v->overlay_user, true);
    SDL_RenderPresent(v->ren);
    v->last_present_ms = now_ms();
}

void video_render(video_t* v, int timeout_ms) {
    frame_buf_t batch[QUEUE_LEN];
    int n = 0;

    /* Wait up to timeout_ms for a frame. A short sleep loop instead of
     * pthread_cond_timedwait: it always returns, so quitting never hangs. */
    for (int waited = 0; waited < timeout_ms; waited++) {
        if (__atomic_load_n(&v->q_count, __ATOMIC_RELAXED) > 0) break;
        svcSleepThread(1000000);
    }
    pthread_mutex_lock(&v->lock);
    while (v->q_count > 0) {
        batch[n++] = v->queue[v->q_head];
        v->queue[v->q_head].data = NULL;
        v->q_head = (v->q_head + 1) % QUEUE_LEN;
        v->q_count--;
    }
    pthread_mutex_unlock(&v->lock);

    bool have_picture = false;
    for (int i = 0; i < n; i++) {
        const double t0 = now_ms();
        v->pkt->data = batch[i].data;
        v->pkt->size = (int)batch[i].size;
        if (avcodec_send_packet(v->dec, v->pkt) == 0) {
            /* Receive into tmp: the last call (no more frames) unrefs its argument,
             * which would wipe the picture we want to show. */
            while (avcodec_receive_frame(v->dec, v->tmp) == 0) {
                av_frame_unref(v->pic);
                av_frame_move_ref(v->pic, v->tmp);
                have_picture = true;
                v->stats.pictures++;
            }
        } else {
            v->stats.decode_errors++;
            mark_lost(v);   /* corrupt data: ask for a fresh keyframe */
        }
        v->decode_total_ms += now_ms() - t0;
        free(batch[i].data);
    }
    /* Several frames in one go means we were late: only the newest is shown. */
    if (have_picture) show_picture(v);
    else if (v->stats.frames_shown == 0) show_waiting(v);
    else if (v->overlay && v->overlay_animating) redraw(v);
}

void video_discard(video_t* v) {
    pthread_mutex_lock(&v->lock);
    while (v->q_count > 0) {
        free(v->queue[v->q_head].data);
        v->queue[v->q_head].data = NULL;
        v->q_head = (v->q_head + 1) % QUEUE_LEN;
        v->q_count--;
    }
    v->need_key = true;   /* frames were skipped: the decoder needs a fresh start */
    pthread_mutex_unlock(&v->lock);
}

const char* video_info(video_t* v) { return v->info; }

void video_get_stats(video_t* v, video_stats_t* out) {
    pthread_mutex_lock(&v->lock);
    *out = v->stats;
    out->need_key = v->need_key;
    pthread_mutex_unlock(&v->lock);
    out->decode_ms = v->stats.frames_received ? (float)(v->decode_total_ms / v->stats.frames_received) : 0;
}
