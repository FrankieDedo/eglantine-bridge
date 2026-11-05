#include "audio.h"

#include <malloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>
#include <switch.h>

#include "ebridge_protocol.h"

/* Delay budget: audout holds AOUT_BUFFERS x 10 ms; our ring keeps between
 * START_MS and MAX_MS on top (the PC and Switch clocks drift a little, and
 * Wi-Fi delivers packets in bursts). */
#define AOUT_BUFFERS 3
#define FRAME_SAMPLES (EB_AUDIO_FRAME * EB_AUDIO_CHANNELS)   /* 960 int16 = 10 ms */
#define FRAME_BYTES (FRAME_SAMPLES * sizeof(int16_t))
#define RING_FRAMES 64                                        /* 640 ms of room */
#define START_MS 30    /* wait for this much before (re)starting after running dry */
#define MAX_MS 120     /* above this, drop old audio down to START_MS */

struct audio {
    OpusDecoder* dec;
    bool have_seq;
    uint32_t next_seq;

    pthread_mutex_t lock;
    int16_t ring[RING_FRAMES][FRAME_SAMPLES];
    int head, count;          /* in 10 ms frames */
    bool prefilling;

    AudioOutBuffer bufs[AOUT_BUFFERS];
    void* mem[AOUT_BUFFERS];
    pthread_t thread;
    bool running;

    audio_stats_t stats;
};

/* Network thread, lock held. */
static void ring_put(audio_t* a, const int16_t* pcm) {
    if (a->count == RING_FRAMES) {   /* full: drop the oldest */
        a->head = (a->head + 1) % RING_FRAMES;
        a->count--;
        a->stats.dropped_ms += 10;
    }
    memcpy(a->ring[(a->head + a->count) % RING_FRAMES], pcm, FRAME_BYTES);
    a->count++;
    if (a->count * 10 > MAX_MS) {    /* too much delay built up: catch up */
        const int drop = a->count - START_MS / 10;
        a->head = (a->head + drop) % RING_FRAMES;
        a->count -= drop;
        a->stats.dropped_ms += (uint32_t)drop * 10;
    }
}

void audio_push_packet(audio_t* a, const uint8_t* payload, size_t len) {
    if (len <= sizeof(eb_audio_t)) return;
    eb_audio_t h;
    memcpy(&h, payload, sizeof h);
    if (h.codec != EB_AUDIO_OPUS || h.channels != EB_AUDIO_CHANNELS || h.samples != EB_AUDIO_FRAME) return;

    int16_t pcm[FRAME_SAMPLES];
    pthread_mutex_lock(&a->lock);
    if (a->have_seq) {
        const int32_t gap = (int32_t)(h.seq - a->next_seq);
        if (gap < 0) {   /* late or duplicate */
            pthread_mutex_unlock(&a->lock);
            return;
        }
        /* A few lost packets: let Opus invent a plausible replacement. */
        for (int32_t i = 0; i < gap && i < 3; i++) {
            if (opus_decode(a->dec, NULL, 0, pcm, EB_AUDIO_FRAME, 0) == EB_AUDIO_FRAME) {
                ring_put(a, pcm);
                a->stats.concealed++;
            }
        }
    }
    a->have_seq = true;
    a->next_seq = h.seq + 1;
    if (opus_decode(a->dec, payload + sizeof h, (opus_int32)(len - sizeof h), pcm, EB_AUDIO_FRAME, 0) ==
        EB_AUDIO_FRAME) {
        ring_put(a, pcm);
        a->stats.packets++;
    }
    pthread_mutex_unlock(&a->lock);
}

/* Playback thread: fills one audout buffer with the next 10 ms, or silence. */
static void fill(audio_t* a, AudioOutBuffer* b) {
    int16_t* out = b->buffer;
    pthread_mutex_lock(&a->lock);
    if (a->prefilling && a->count * 10 >= START_MS) a->prefilling = false;
    if (!a->prefilling && a->count > 0) {
        memcpy(out, a->ring[a->head], FRAME_BYTES);
        a->head = (a->head + 1) % RING_FRAMES;
        a->count--;
    } else {
        memset(out, 0, FRAME_BYTES);
        if (!a->prefilling && a->stats.packets > 0) {
            a->stats.underruns++;
            a->prefilling = true;   /* build a little margin again before resuming */
        }
    }
    pthread_mutex_unlock(&a->lock);
    b->data_size = FRAME_BYTES;
    b->data_offset = 0;
}

static void* playback_main(void* arg) {
    audio_t* a = arg;
    for (int i = 0; i < AOUT_BUFFERS; i++) {
        fill(a, &a->bufs[i]);
        audoutAppendAudioOutBuffer(&a->bufs[i]);
    }
    while (__atomic_load_n(&a->running, __ATOMIC_RELAXED)) {
        AudioOutBuffer* released = NULL;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL)) || !count) continue;
        while (released) {
            fill(a, released);
            audoutAppendAudioOutBuffer(released);
            released = NULL;
            if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || !count) break;
        }
    }
    return NULL;
}

audio_t* audio_create(void) {
    audio_t* a = calloc(1, sizeof *a);
    if (!a) return NULL;
    int err = 0;
    a->dec = opus_decoder_create(EB_AUDIO_RATE, EB_AUDIO_CHANNELS, &err);
    if (!a->dec) goto fail_free;
    if (R_FAILED(audoutInitialize())) goto fail_dec;
    if (audoutGetSampleRate() != EB_AUDIO_RATE || audoutGetChannelCount() != EB_AUDIO_CHANNELS ||
        R_FAILED(audoutStartAudioOut()))
        goto fail_aout;
    for (int i = 0; i < AOUT_BUFFERS; i++) {
        a->mem[i] = memalign(0x1000, 0x1000);   /* audout wants 4 KB aligned buffers */
        if (!a->mem[i]) goto fail_started;
        memset(a->mem[i], 0, 0x1000);
        a->bufs[i].buffer = a->mem[i];
        a->bufs[i].buffer_size = 0x1000;
    }
    pthread_mutex_init(&a->lock, NULL);
    a->prefilling = true;
    a->running = true;
    if (pthread_create(&a->thread, NULL, playback_main, a) != 0) {
        pthread_mutex_destroy(&a->lock);
        goto fail_started;
    }
    return a;

fail_started:
    for (int i = 0; i < AOUT_BUFFERS; i++) free(a->mem[i]);
    audoutStopAudioOut();
fail_aout:
    audoutExit();
fail_dec:
    opus_decoder_destroy(a->dec);
fail_free:
    free(a);
    return NULL;
}

void audio_destroy(audio_t* a) {
    if (!a) return;
    __atomic_store_n(&a->running, false, __ATOMIC_RELAXED);
    pthread_join(a->thread, NULL);
    audoutStopAudioOut();
    bool flushed;
    audoutFlushAudioOutBuffers(&flushed);
    audoutExit();
    for (int i = 0; i < AOUT_BUFFERS; i++) free(a->mem[i]);
    opus_decoder_destroy(a->dec);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

void audio_get_stats(audio_t* a, audio_stats_t* out) {
    pthread_mutex_lock(&a->lock);
    *out = a->stats;
    out->buffered_ms = (uint32_t)a->count * 10;
    pthread_mutex_unlock(&a->lock);
}
