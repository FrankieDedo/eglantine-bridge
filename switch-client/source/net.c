#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

#define SESSION_TIMEOUT_NS 5000000000ULL

static uint32_t now_us(void) {
    return (uint32_t)(armTicksToNs(armGetSystemTick()) / 1000);
}

static bool wait_readable(int fd, int timeout_ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
}

static bool recv_exact(int fd, void* buf, size_t n) {
    char* p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool ctl_send(int fd, uint16_t type, const void* payload, uint16_t len) {
    uint8_t buf[sizeof(eb_ctl_header_t) + 128];
    if (len > 128) return false;
    eb_ctl_header_t h = {type, len};
    memcpy(buf, &h, sizeof h);
    if (len) memcpy(buf + sizeof h, payload, len);
    return send(fd, buf, sizeof h + len, 0) == (ssize_t)(sizeof h + len);
}

/* Reads one control message; payload is truncated to cap bytes. */
static bool ctl_recv(int fd, uint16_t* type, void* payload, uint16_t cap, uint16_t* len) {
    eb_ctl_header_t h;
    if (!recv_exact(fd, &h, sizeof h)) return false;
    uint8_t scratch[sizeof(eb_ctl_game_icon_t) + EB_ICON_CHUNK + 64];   /* largest message */
    if (h.length > sizeof scratch || !recv_exact(fd, scratch, h.length)) return false;
    memcpy(payload, scratch, h.length < cap ? h.length : cap);
    *type = h.type;
    if (len) *len = h.length;
    return true;
}

static void udp_header(eb_udp_header_t* h, uint8_t type, uint32_t token, uint32_t seq) {
    h->magic = EB_UDP_MAGIC;
    h->type = type;
    h->flags = 0;
    h->session_token = token;
    h->seq = seq;
    h->timestamp_us = now_us();
}

int eb_discover(eb_found_host_t* out, int max, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

    eb_disc_query_t q = {EB_MAGIC, EB_DISC_QUERY, EB_PROTO_VERSION, 0};
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(EB_DISCOVERY_PORT)};

    /* Directed broadcast (e.g. 192.168.1.255) works on more routers than 255.255.255.255. */
    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2)) && mask) {
        to.sin_addr.s_addr = ip | ~mask;
        sendto(fd, &q, sizeof q, 0, (struct sockaddr*)&to, sizeof to);
    }
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, &q, sizeof q, 0, (struct sockaddr*)&to, sizeof to);

    int count = 0;
    u64 end = armGetSystemTick() + armNsToTicks((u64)timeout_ms * 1000000ULL);
    while (count < max && armGetSystemTick() < end) {
        if (!wait_readable(fd, 50)) continue;
        eb_disc_response_t r;
        struct sockaddr_in from;
        socklen_t from_len = sizeof from;
        ssize_t n = recvfrom(fd, &r, sizeof r, 0, (struct sockaddr*)&from, &from_len);
        if (n < (ssize_t)sizeof r || r.magic != EB_MAGIC || r.type != EB_DISC_RESPONSE) continue;

        eb_found_host_t h = {0};
        inet_ntop(AF_INET, &from.sin_addr, h.ip, sizeof h.ip);
        h.port = r.port;
        memcpy(h.name, r.name, EB_NAME_LEN);
        h.flags = r.flags;
        bool dup = false;   /* both broadcasts may reach the same host */
        for (int i = 0; i < count; i++)
            if (!strcmp(out[i].ip, h.ip) && out[i].port == h.port) dup = true;
        if (!dup) out[count++] = h;
    }
    close(fd);
    return count;
}

static bool resolve(const char* host, uint16_t port, struct sockaddr_in* out) {
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) return true;
    struct addrinfo hints = {.ai_family = AF_INET}, *res = NULL;   /* allows hostnames / DDNS */
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static const char* reject_text(uint16_t reason) {
    switch (reason) {
    case EB_REJECT_BAD_VERSION: return "different version: update the app and the PC side";
    case EB_REJECT_BAD_PIN: return "wrong PIN";
    case EB_REJECT_BUSY: return "the PC is already connected to another console";
    default: return "protocol error";
    }
}

bool eb_connect(eb_session_t* s, const char* ip, uint16_t port, const uint8_t client_id[EB_ID_LEN],
                uint8_t secret[EB_ID_LEN], bool* paired_now, char* host_name_out,
                eb_pin_prompt_fn prompt, char* err, size_t err_len) {
    (void)host_name_out;
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->status_lock, NULL);
    s->tcp = s->udp = -1;
    *paired_now = false;

    struct sockaddr_in addr;
    if (!resolve(ip, port, &addr)) {
        snprintf(err, err_len, "invalid address: %s", ip);
        return false;
    }
    s->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->tcp < 0 || connect(s->tcp, (struct sockaddr*)&addr, sizeof addr) != 0) {
        snprintf(err, err_len, "cannot connect to %s:%u (is Eglantine open?)", ip, port);
        eb_close(s, false);
        return false;
    }
    int one = 1;
    setsockopt(s->tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    eb_ctl_hello_t hello = {0};
    hello.magic = EB_MAGIC;
    hello.version = EB_PROTO_VERSION;
    memcpy(hello.client_id, client_id, EB_ID_LEN);
    memcpy(hello.pair_secret, secret, EB_ID_LEN);
    snprintf(hello.name, sizeof hello.name, "Nintendo Switch");
    /* Software decoding for now: 720p60 H.264 fits on the 4 CPU cores.
     * Hardware decoding (NVDEC) will allow 1080p and HEVC. */
    hello.max_width = 1280;
    hello.max_height = 720;
    hello.codecs = EB_CODEC_H264;
    hello.max_fps = 60;
    if (!ctl_send(s->tcp, EB_CTL_HELLO, &hello, sizeof hello)) goto io_error;

    for (;;) {
        uint16_t type;
        uint8_t payload[64];
        if (!wait_readable(s->tcp, 130000) || !ctl_recv(s->tcp, &type, payload, sizeof payload, NULL))
            goto io_error;
        if (type == EB_CTL_PAIR_REQUIRED) {
            eb_ctl_pair_pin_t p;
            if (!prompt(&p.pin)) {
                snprintf(err, err_len, "pairing cancelled");
                eb_close(s, true);
                return false;
            }
            if (!ctl_send(s->tcp, EB_CTL_PAIR_PIN, &p, sizeof p)) goto io_error;
        } else if (type == EB_CTL_PAIR_OK) {
            eb_ctl_pair_ok_t ok;
            memcpy(&ok, payload, sizeof ok);
            memcpy(secret, ok.pair_secret, EB_ID_LEN);
            *paired_now = true;
        } else if (type == EB_CTL_REJECT) {
            eb_ctl_reject_t r;
            memcpy(&r, payload, sizeof r);
            snprintf(err, err_len, "refused: %s", reject_text(r.reason));
            eb_close(s, false);
            return false;
        } else if (type == EB_CTL_WELCOME) {
            memcpy(&s->welcome, payload, sizeof s->welcome);
            break;
        }
    }

    s->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->udp < 0) goto io_error;
    fcntl(s->udp, F_SETFL, fcntl(s->udp, F_GETFL, 0) | O_NONBLOCK);
    int rcvbuf = 1024 * 1024;   /* a keyframe arrives as ~100 packets at once */
    setsockopt(s->udp, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    s->udp_dest = addr;
    s->udp_dest.sin_port = htons(s->welcome.udp_port);

    eb_udp_header_t h;
    udp_header(&h, EB_UDP_HELLO, s->welcome.session_token, 0);
    sendto(s->udp, &h, sizeof h, 0, (struct sockaddr*)&s->udp_dest, sizeof s->udp_dest);
    s->last_rx_tick = armGetSystemTick();
    return true;

io_error:
    snprintf(err, err_len, "connection lost while starting");
    eb_close(s, false);
    return false;
}

void eb_send_input(eb_session_t* s, const eb_input_t* in) {
    uint8_t buf[sizeof(eb_udp_header_t) + sizeof(eb_input_t)];
    eb_udp_header_t h;
    udp_header(&h, EB_UDP_INPUT, s->welcome.session_token, ++s->input_seq);
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, in, sizeof *in);
    sendto(s->udp, buf, sizeof buf, 0, (struct sockaddr*)&s->udp_dest, sizeof s->udp_dest);
}

void eb_send_ping(eb_session_t* s) {
    eb_ctl_ping_t p = {armTicksToNs(armGetSystemTick()) / 1000};
    ctl_send(s->tcp, EB_CTL_PING, &p, sizeof p);
}

void eb_request_idr(eb_session_t* s) {
    ctl_send(s->tcp, EB_CTL_REQUEST_IDR, NULL, 0);
}

void eb_send_heartbeat(eb_session_t* s) {
    eb_udp_header_t h;
    udp_header(&h, EB_UDP_HEARTBEAT, s->welcome.session_token, 0);
    sendto(s->udp, &h, sizeof h, 0, (struct sockaddr*)&s->udp_dest, sizeof s->udp_dest);
}

static void store_status(eb_session_t* s, const uint8_t* payload) {
    pthread_mutex_lock(&s->status_lock);
    memcpy(&s->status, payload, sizeof s->status);
    s->status.text[sizeof s->status.text - 1] = 0;
    s->status.game[sizeof s->status.game - 1] = 0;
    s->have_status = true;
    pthread_mutex_unlock(&s->status_lock);
}

bool eb_get_status(eb_session_t* s, eb_ctl_status_t* out) {
    pthread_mutex_lock(&s->status_lock);
    const bool have = s->have_status;
    if (have) *out = s->status;
    pthread_mutex_unlock(&s->status_lock);
    return have;
}

static void copy_field(char* dst, const char* src, size_t n) {
    memcpy(dst, src, n);
    dst[n - 1] = 0;
}

int eb_list_games(eb_session_t* s, eb_game_t* out, int max, int timeout_ms) {
    const eb_ctl_games_request_t req = {EB_GAMES_WANT_ICONS};
    if (!ctl_send(s->tcp, EB_CTL_GAMES_REQUEST, &req, sizeof req)) return -1;
    int count = 0;
    /* The deadline moves forward while data keeps coming: icons can take a moment. */
    u64 end = armTicksToNs(armGetSystemTick()) + (u64)timeout_ms * 1000000ULL;
    u64 last_beat = 0;
    while (armTicksToNs(armGetSystemTick()) < end) {
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now - last_beat > 500000000ULL) {
            eb_send_heartbeat(s);
            last_beat = now;
        }
        if (!wait_readable(s->tcp, 200)) continue;
        uint16_t type, len;
        uint8_t payload[sizeof(eb_ctl_game_icon_t) + EB_ICON_CHUNK];
        if (!ctl_recv(s->tcp, &type, payload, sizeof payload, &len)) return -1;
        __atomic_store_n(&s->last_rx_tick, armGetSystemTick(), __ATOMIC_RELAXED);
        end = armTicksToNs(armGetSystemTick()) + (u64)timeout_ms * 1000000ULL;
        if (type == EB_CTL_GAME_ENTRY && len >= 112 && count < max) {
            /* Older hosts send only the first 112 bytes (no columns). */
            eb_ctl_game_t g = {0};
            memcpy(&g, payload, len < sizeof g ? len : sizeof g);
            eb_game_t* o = &out[count];
            memset(o, 0, sizeof *o);
            o->index = g.index;
            o->running = (g.flags & EB_GAME_RUNNING) != 0;
            o->title_id = g.title_id;
            copy_field(o->name, g.name, sizeof o->name);
            copy_field(o->file_type, g.file_type, sizeof o->file_type);
            copy_field(o->size, g.size, sizeof o->size);
            copy_field(o->play_time, g.play_time, sizeof o->play_time);
            count++;
        } else if (type == EB_CTL_GAME_ICON && len > sizeof(eb_ctl_game_icon_t)) {
            eb_ctl_game_icon_t h;
            memcpy(&h, payload, sizeof h);
            const uint32_t n = len - sizeof h;
            for (int i = 0; i < count; i++) {
                if (out[i].index != h.index || h.size != EB_ICON_SIZE) continue;
                if (h.offset + n <= EB_ICON_BYTES) {
                    memcpy(out[i].icon + h.offset, payload + sizeof h, n);
                    out[i].icon_bytes += n;
                }
            }
        } else if (type == EB_CTL_STATUS && len >= sizeof(eb_ctl_status_t)) {
            store_status(s, payload);
        } else if (type == EB_CTL_GAMES_END) {
            return count;
        }
    }
    return -1;   /* no answer: an older host without the game list */
}

void eb_launch_game(eb_session_t* s, uint16_t index) {
    eb_ctl_launch_t l = {index, 0};
    ctl_send(s->tcp, EB_CTL_LAUNCH, &l, sizeof l);
}

int eb_recv_udp(eb_session_t* s, uint8_t* buf, size_t cap, int timeout_ms) {
    if (!wait_readable(s->udp, timeout_ms)) return 0;
    for (;;) {
        ssize_t n = recv(s->udp, buf, cap, 0);
        if (n < (ssize_t)sizeof(eb_udp_header_t)) return 0;
        eb_udp_header_t h;
        memcpy(&h, buf, sizeof h);
        if (h.magic != EB_UDP_MAGIC || h.session_token != s->welcome.session_token) continue;
        __atomic_store_n(&s->last_rx_tick, armGetSystemTick(), __ATOMIC_RELAXED);
        return (int)n;
    }
}

bool eb_poll_control(eb_session_t* s) {
    while (wait_readable(s->tcp, 0)) {
        uint16_t type, len;
        uint8_t payload[sizeof(eb_ctl_status_t) + 16];
        if (!ctl_recv(s->tcp, &type, payload, sizeof payload, &len)) return false;
        __atomic_store_n(&s->last_rx_tick, armGetSystemTick(), __ATOMIC_RELAXED);
        if (type == EB_CTL_PONG && len == sizeof(eb_ctl_ping_t)) {
            eb_ctl_ping_t p;
            memcpy(&p, payload, sizeof p);
            s->last_rtt_us = (uint32_t)(armTicksToNs(armGetSystemTick()) / 1000 - p.client_time_us);
        } else if (type == EB_CTL_STATUS && len >= sizeof(eb_ctl_status_t)) {
            store_status(s, payload);
        } else if (type == EB_CTL_BYE) {
            return false;
        }
    }
    const u64 last = __atomic_load_n(&s->last_rx_tick, __ATOMIC_RELAXED);
    return armTicksToNs(armGetSystemTick() - last) < SESSION_TIMEOUT_NS;
}

void eb_close(eb_session_t* s, bool say_bye) {
    if (s->tcp >= 0) {
        if (say_bye) ctl_send(s->tcp, EB_CTL_BYE, NULL, 0);
        close(s->tcp);
    }
    if (s->udp >= 0) close(s->udp);
    s->tcp = s->udp = -1;
}
