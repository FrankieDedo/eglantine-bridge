// Networking for the Switch client: discovery, control channel, UDP stream.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>
#include "ebridge_protocol.h"

typedef struct {
    char     ip[64];
    uint16_t port;
    char     name[EB_NAME_LEN + 1];
    uint32_t flags;      /* EB_HOST_FLAG_* */
} eb_found_host_t;

typedef struct {
    int tcp, udp;
    struct sockaddr_in udp_dest;
    eb_ctl_welcome_t welcome;
    uint32_t input_seq;
    uint32_t last_rtt_us;
    uint64_t last_rx_tick;   /* last packet from host, for timeouts */
    /* What the emulator is doing: written by whichever thread reads the control
     * channel, read by the one drawing the screen. */
    pthread_mutex_t status_lock;
    eb_ctl_status_t status;
    bool have_status;
} eb_session_t;

/* Copies the latest emulator status. Returns false if the PC never sent one
 * (older version). */
bool eb_get_status(eb_session_t* s, eb_ctl_status_t* out);

/* Asks the user for the PIN shown on the PC. Return false if cancelled. */
typedef bool (*eb_pin_prompt_fn)(uint32_t* pin_out);

/* Broadcasts on the LAN and collects answers for timeout_ms. */
int eb_discover(eb_found_host_t* out, int max, int timeout_ms);

/* Connects and pairs if needed. secret is read and, after a new pairing,
 * overwritten (*paired_now = true): save it. Returns false + err on failure. */
bool eb_connect(eb_session_t* s, const char* ip, uint16_t port, const uint8_t client_id[EB_ID_LEN],
                uint8_t secret[EB_ID_LEN], bool* paired_now, char* host_name_out,
                eb_pin_prompt_fn prompt, char* err, size_t err_len);

void eb_send_input(eb_session_t* s, const eb_input_t* in);
void eb_send_ping(eb_session_t* s);
void eb_request_idr(eb_session_t* s);

/* Keeps the session alive while no input is sent (menus). */
void eb_send_heartbeat(eb_session_t* s);

typedef struct {
    uint16_t index;
    bool     running;
    uint64_t title_id;
    char     name[EB_GAME_NAME_LEN];   /* UTF-8 */
    char     file_type[16], size[16], play_time[16];
    uint8_t  icon[EB_ICON_BYTES];      /* RGB888, valid if icon_bytes == EB_ICON_BYTES */
    uint32_t icon_bytes;
} eb_game_t;

/* Asks the PC for the games it can start. Returns how many were received,
 * or -1 if the host doesn't answer (older version without a game list). */
int eb_list_games(eb_session_t* s, eb_game_t* out, int max, int timeout_ms);
void eb_launch_game(eb_session_t* s, uint16_t index);

/* Control channel, non-blocking: handles PONG/BYE. Returns false when the
 * session is over (closed by the PC or 5 s without any packet). */
bool eb_poll_control(eb_session_t* s);

/* Waits up to timeout_ms for one valid stream packet (magic + token checked).
 * Returns its length (header included), or 0 on timeout. */
int eb_recv_udp(eb_session_t* s, uint8_t* buf, size_t cap, int timeout_ms);

void eb_close(eb_session_t* s, bool say_bye);
