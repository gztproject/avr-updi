#pragma once

#include "updi_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct UpdiLink {
    UpdiTransport* transport;
    char* error;
    size_t error_size;
} UpdiLink;

void updi_link_init(UpdiLink* link, UpdiTransport* transport, char* error, size_t error_size);

bool updi_link_startup(UpdiLink* link);
bool updi_link_read_sib(UpdiLink* link, uint8_t sib[32]);
bool updi_link_try_fast_baud(UpdiLink* link);

bool updi_link_ldcs(UpdiLink* link, uint8_t address, uint8_t* value);
bool updi_link_stcs(UpdiLink* link, uint8_t address, uint8_t value);
bool updi_link_key(UpdiLink* link, const uint8_t key[8]);

bool updi_link_ld8(UpdiLink* link, uint32_t address, uint8_t* value);
bool updi_link_st8(UpdiLink* link, uint32_t address, uint8_t value);
bool updi_link_read(UpdiLink* link, uint32_t address, uint8_t* data, size_t size);
bool updi_link_write_words(UpdiLink* link, uint32_t address, const uint8_t* data, size_t size);
