#pragma once

#include "updi_link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool updi_nvm_wait(UpdiLink* link, uint32_t timeout_ms);
bool updi_nvm_command(UpdiLink* link, uint8_t command);
bool updi_nvm_chip_erase(UpdiLink* link);
bool updi_nvm_write_flash_page(UpdiLink* link, uint32_t byte_address, const uint8_t* page);
bool updi_nvm_read_flash(UpdiLink* link, uint32_t byte_address, uint8_t* data, size_t size);
