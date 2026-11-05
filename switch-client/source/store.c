#include "store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#define STORE_DIR  "sdmc:/config/eglantine-bridge"
#define STORE_FILE STORE_DIR "/hosts.txt"

static void to_hex(const uint8_t* p, int n, char* out) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, "%02x", p[i]);
}

static bool from_hex(const char* s, uint8_t* out, int n) {
    if ((int)strlen(s) < n * 2) return false;
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* File format:
 *   id <client_id hex>
 *   host <ip> <port> <secret hex> <name...>          */
void store_load(store_t* s) {
    memset(s, 0, sizeof *s);
    bool have_id = false;
    FILE* f = fopen(STORE_FILE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char hex[64];
            if (sscanf(line, "id %63s", hex) == 1) {
                have_id = from_hex(hex, s->client_id, EB_ID_LEN);
            } else if (!strncmp(line, "host ", 5) && s->host_count < STORE_MAX_HOSTS) {
                saved_host_t* h = &s->hosts[s->host_count];
                unsigned port;
                int name_at = 0;
                if (sscanf(line, "host %63s %u %63s %n", h->ip, &port, hex, &name_at) >= 3) {
                    h->port = (uint16_t)port;
                    from_hex(hex, h->secret, EB_ID_LEN);
                    if (name_at > 0) snprintf(h->name, sizeof h->name, "%s", line + name_at);
                    s->host_count++;
                }
            }
        }
        fclose(f);
    }
    if (!have_id) {
        randomGet(s->client_id, EB_ID_LEN);
        store_save(s);
    }
}

void store_save(const store_t* s) {
    mkdir("sdmc:/config", 0777);
    mkdir(STORE_DIR, 0777);
    FILE* f = fopen(STORE_FILE, "w");
    if (!f) return;
    char hex[EB_ID_LEN * 2 + 1];
    to_hex(s->client_id, EB_ID_LEN, hex);
    fprintf(f, "id %s\n", hex);
    for (int i = 0; i < s->host_count; i++) {
        const saved_host_t* h = &s->hosts[i];
        to_hex(h->secret, EB_ID_LEN, hex);
        fprintf(f, "host %s %u %s %s\n", h->ip, h->port, hex, h->name);
    }
    fclose(f);
}

saved_host_t* store_get_host(store_t* s, const char* ip, uint16_t port) {
    int found = -1;
    for (int i = 0; i < s->host_count; i++)
        if (s->hosts[i].port == port && !strcmp(s->hosts[i].ip, ip)) found = i;

    saved_host_t entry;
    if (found >= 0) {
        entry = s->hosts[found];
    } else {
        memset(&entry, 0, sizeof entry);
        snprintf(entry.ip, sizeof entry.ip, "%s", ip);
        entry.port = port;
        found = s->host_count < STORE_MAX_HOSTS ? s->host_count++ : STORE_MAX_HOSTS - 1;
    }
    /* Move to the front so the last used PC is listed first. */
    memmove(&s->hosts[1], &s->hosts[0], sizeof(saved_host_t) * found);
    s->hosts[0] = entry;
    return &s->hosts[0];
}
