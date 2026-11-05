// Audio: decodes the PC's Opus packets and plays them with audout.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct audio audio_t;

typedef struct {
    uint32_t packets;       /* Opus packets received */
    uint32_t concealed;     /* lost packets replaced by Opus concealment */
    uint32_t underruns;     /* times the buffer ran dry (crackle) */
    uint32_t dropped_ms;    /* audio thrown away to keep the delay low */
    uint32_t buffered_ms;   /* current delay in our buffer */
} audio_stats_t;

/* Opens audout and starts the playback thread. NULL if audio isn't available. */
audio_t* audio_create(void);
void audio_destroy(audio_t* a);

/* Network thread: one EB_UDP_AUDIO payload (eb_audio_t + Opus packet). */
void audio_push_packet(audio_t* a, const uint8_t* payload, size_t len);

void audio_get_stats(audio_t* a, audio_stats_t* out);
