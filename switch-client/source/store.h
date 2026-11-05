// Saved data on the SD card: this console's id and the PCs it is paired with.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ebridge_protocol.h"

#define STORE_MAX_HOSTS 16

typedef struct {
    char     ip[64];
    uint16_t port;
    char     name[EB_NAME_LEN + 1];
    uint8_t  secret[EB_ID_LEN];   /* all zero = not paired yet */
} saved_host_t;

typedef struct {
    uint8_t      client_id[EB_ID_LEN];
    saved_host_t hosts[STORE_MAX_HOSTS];
    int          host_count;
} store_t;

void store_load(store_t* s);   /* creates a client id on first run */
void store_save(const store_t* s);

/* Finds ip:port, or adds it (most recent first). Never returns NULL. */
saved_host_t* store_get_host(store_t* s, const char* ip, uint16_t port);
