#include "updi_nvm.h"

#include "updi_constants.h"

#include <furi.h>

#include <stdio.h>

static void updi_nvm_error_code(UpdiLink* link, uint8_t code) {
    const char* name = "error";
    if(code == 1) name = "invalid command";
    else if(code == 2) name = "write protected";
    else if(code == 3) name = "command collision";
    snprintf(link->error, link->error_size, "NVM %s (%u)", name, code);
}

bool updi_nvm_wait(UpdiLink* link, uint32_t timeout_ms) {
    uint32_t start = furi_get_tick();
    uint32_t limit = furi_ms_to_ticks(timeout_ms);
    while(true) {
        uint8_t status = 0;
        if(!updi_link_ld8(link, UPDI_NVMCTRL_BASE + UPDI_NVM_STATUS, &status)) return false;
        uint8_t code = (uint8_t)((status & UPDI_NVM_STATUS_ERROR_MASK) >> UPDI_NVM_STATUS_ERROR_POS);
        if(code != 0) {
            updi_nvm_error_code(link, code);
            return false;
        }
        if((status & (UPDI_NVM_STATUS_EEBUSY | UPDI_NVM_STATUS_FBUSY)) == 0) return true;
        if((furi_get_tick() - start) >= limit) {
            snprintf(link->error, link->error_size, "NVM busy timeout");
            return false;
        }
        furi_delay_ms(1);
    }
}

bool updi_nvm_command(UpdiLink* link, uint8_t command) {
    return updi_link_st8(link, UPDI_NVMCTRL_BASE + UPDI_NVM_CTRLA, command);
}

bool updi_nvm_chip_erase(UpdiLink* link) {
    if(!updi_nvm_wait(link, 500)) return false;
    if(!updi_nvm_command(link, UPDI_NVM_CHER)) return false;
    if(!updi_nvm_wait(link, 2000)) return false;
    return updi_nvm_command(link, UPDI_NVM_NOCMD);
}

bool updi_nvm_write_flash_page(UpdiLink* link, uint32_t byte_address, const uint8_t* page) {
    if((byte_address % UPDI_FLASH_PAGE) != 0 || byte_address >= UPDI_FLASH_SIZE) {
        snprintf(link->error, link->error_size, "Bad flash page");
        return false;
    }

    uint32_t address = UPDI_FLASH_BASE + byte_address;
    if(!updi_nvm_wait(link, 500)) return false;
    if(!updi_nvm_command(link, UPDI_NVM_FLPER)) return false;
    if(!updi_link_st8(link, address, 0xFF)) return false;
    if(!updi_nvm_wait(link, 500)) return false;
    if(!updi_nvm_command(link, UPDI_NVM_NOCMD)) return false;

    if(!updi_nvm_wait(link, 500)) return false;
    if(!updi_nvm_command(link, UPDI_NVM_FLWR)) return false;
    for(uint32_t offset = 0; offset < UPDI_FLASH_PAGE; offset += 64) {
        if(!updi_link_write_words(link, address + offset, page + offset, 64)) return false;
        if(!updi_nvm_wait(link, 500)) return false;
    }
    return updi_nvm_command(link, UPDI_NVM_NOCMD);
}

bool updi_nvm_read_flash(UpdiLink* link, uint32_t byte_address, uint8_t* data, size_t size) {
    while(size > 0) {
        size_t chunk = size > UPDI_MAX_REPEAT ? UPDI_MAX_REPEAT : size;
        if(!updi_link_read(link, UPDI_FLASH_BASE + byte_address, data, chunk)) return false;
        byte_address += (uint32_t)chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}
