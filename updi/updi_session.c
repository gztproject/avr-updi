#include "updi_session.h"

#include "updi_constants.h"
#include "updi_link.h"
#include "updi_nvm.h"
#include "updi_transport.h"

#include <furi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct UpdiSession {
    UpdiTransport* transport;
    UpdiLink link;
    char error[160];
    bool open;
    bool prog_mode;
    UpdiTargetInfo target;
};

static const uint8_t updi_key_nvm[8] = {'N', 'V', 'M', 'P', 'r', 'o', 'g', ' '};
static const uint8_t updi_key_erase[8] = {'N', 'V', 'M', 'E', 'r', 'a', 's', 'e'};

static void updi_copy_sib(UpdiTargetInfo* info, const uint8_t sib[UPDI_SIB_SIZE]) {
    size_t end = 0;
    for(size_t i = 0; i < UPDI_SIB_SIZE; i++) {
        char c = (char)sib[i];
        if(c >= 32 && c < 127) {
            info->sib[i] = c;
            end = i + 1;
        } else {
            info->sib[i] = ' ';
        }
    }
    info->sib[end] = '\0';
    while(end > 0 && info->sib[end - 1] == ' ') {
        end--;
        info->sib[end] = '\0';
    }
}

UpdiSession* updi_session_alloc(void) {
    UpdiSession* session = calloc(1, sizeof(UpdiSession));
    if(session == NULL) return NULL;
    session->transport = updi_transport_alloc();
    if(session->transport == NULL) {
        free(session);
        return NULL;
    }
    updi_link_init(&session->link, session->transport, session->error, sizeof(session->error));
    return session;
}

void updi_session_set_pins(UpdiSession* session, UpdiPins pins) {
    updi_transport_set_pins(session->transport, pins);
}

void updi_session_free(UpdiSession* session) {
    if(session == NULL) return;
    updi_session_disconnect(session);
    updi_transport_free(session->transport);
    free(session);
}

bool updi_session_enter_progmode(UpdiSession* session) {
    uint8_t sys = 0;
    if(!updi_link_ldcs(&session->link, UPDI_ASI_SYS_STATUS, &sys)) return false;
    if((sys & (1u << UPDI_ASI_SYS_STATUS_NVMPROG)) != 0) {
        session->prog_mode = true;
        session->target.locked = false;
        return true;
    }

    if(!updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, UPDI_RESET_REQ_VALUE)) return false;
    if(!updi_link_key(&session->link, updi_key_nvm)) return false;

    uint8_t key_status = 0;
    if(!updi_link_ldcs(&session->link, UPDI_ASI_KEY_STATUS, &key_status)) return false;
    if((key_status & (1u << UPDI_ASI_KEY_STATUS_NVMPROG)) == 0) {
        updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, 0x00);
        snprintf(session->error, sizeof(session->error), "NVMProg key rejected");
        return false;
    }

    if(!updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, UPDI_RESET_REQ_VALUE)) return false;
    if(!updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, 0x00)) return false;
    furi_delay_ms(10);

    uint32_t start = furi_get_tick();
    bool saw_lock = false;
    while((furi_get_tick() - start) < furi_ms_to_ticks(300)) {
        if(!updi_link_ldcs(&session->link, UPDI_ASI_SYS_STATUS, &sys)) return false;
        if((sys & (1u << UPDI_ASI_SYS_STATUS_LOCKSTATUS)) == 0) {
            if((sys & (1u << UPDI_ASI_SYS_STATUS_NVMPROG)) == 0) {
                snprintf(session->error, sizeof(session->error), "Did not enter prog mode");
                return false;
            }
            session->prog_mode = true;
            session->target.locked = false;
            return true;
        }
        saw_lock = true;
        furi_delay_ms(5);
    }

    session->target.locked = saw_lock;
    snprintf(
        session->error,
        sizeof(session->error),
        saw_lock ? "Device is locked" : "Did not enter prog mode");
    return false;
}

bool updi_session_unlock(UpdiSession* session) {
    if(!updi_link_key(&session->link, updi_key_erase)) return false;

    uint8_t key_status = 0;
    if(!updi_link_ldcs(&session->link, UPDI_ASI_KEY_STATUS, &key_status)) return false;
    if((key_status & (1u << UPDI_ASI_KEY_STATUS_CHIPERASE)) == 0) {
        snprintf(session->error, sizeof(session->error), "Erase key rejected");
        return false;
    }

    if(!updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, UPDI_RESET_REQ_VALUE)) return false;
    if(!updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, 0x00)) return false;
    furi_delay_ms(20);

    uint32_t start = furi_get_tick();
    while((furi_get_tick() - start) < furi_ms_to_ticks(1000)) {
        uint8_t sys = 0;
        if(!updi_link_ldcs(&session->link, UPDI_ASI_SYS_STATUS, &sys)) return false;
        if((sys & (1u << UPDI_ASI_SYS_STATUS_LOCKSTATUS)) == 0) {
            session->target.locked = false;
            return true;
        }
        furi_delay_ms(10);
    }

    snprintf(session->error, sizeof(session->error), "Chip erase key timed out");
    return false;
}

bool updi_session_read_signature(UpdiSession* session) {
    uint8_t signature[3];
    if(!updi_link_read(&session->link, UPDI_SIGROW_BASE, signature, sizeof(signature))) return false;
    updi_target_set_signature(&session->target, signature);
    return true;
}

UpdiConnectStatus updi_session_connect(UpdiSession* session) {
    session->error[0] = '\0';
    memset(&session->target, 0, sizeof(session->target));
    session->prog_mode = false;

    if(!updi_transport_open(session->transport)) {
        snprintf(
            session->error, sizeof(session->error), "%s", updi_transport_error(session->transport));
        return UpdiConnectFail;
    }
    session->open = true;

    if(!updi_link_startup(&session->link)) return UpdiConnectFail;

    uint8_t sib[UPDI_SIB_SIZE];
    if(!updi_link_read_sib(&session->link, sib)) return UpdiConnectFail;
    updi_copy_sib(&session->target, sib);

    if(!(sib[UPDI_SIB_NVM_INDEX] == 'P' && sib[UPDI_SIB_NVM_INDEX + 1] == ':' &&
         sib[UPDI_SIB_NVM_INDEX + 2] == '2')) {
        char nvm[4] = {
            (char)sib[UPDI_SIB_NVM_INDEX],
            (char)sib[UPDI_SIB_NVM_INDEX + 1],
            (char)sib[UPDI_SIB_NVM_INDEX + 2],
            '\0',
        };
        snprintf(session->target.nvm, sizeof(session->target.nvm), "%s", nvm);
        snprintf(session->error, sizeof(session->error), "Unsupported NVM %s", nvm);
        return UpdiConnectFail;
    }
    snprintf(session->target.nvm, sizeof(session->target.nvm), "P:2");

    if(!updi_link_try_fast_baud(&session->link)) return UpdiConnectFail;
    if(!updi_session_enter_progmode(session)) {
        if(session->target.locked) return UpdiConnectLocked;
        return UpdiConnectFail;
    }
    if(!updi_session_read_signature(session)) return UpdiConnectFail;

    FURI_LOG_I(
        "Updi",
        "Connected %s %02X%02X%02X at %lu",
        session->target.name,
        session->target.signature[0],
        session->target.signature[1],
        session->target.signature[2],
        (unsigned long)updi_transport_baud(session->transport));
    return UpdiConnectOk;
}

bool updi_session_chip_erase(UpdiSession* session) {
    if(!session->prog_mode) {
        snprintf(session->error, sizeof(session->error), "Not in prog mode");
        return false;
    }
    return updi_nvm_chip_erase(&session->link);
}

bool updi_session_write_page(UpdiSession* session, uint32_t byte_address, const uint8_t* page) {
    if(!session->prog_mode) {
        snprintf(session->error, sizeof(session->error), "Not in prog mode");
        return false;
    }
    return updi_nvm_write_flash_page(&session->link, byte_address, page);
}

bool updi_session_read_flash(
    UpdiSession* session,
    uint32_t byte_address,
    uint8_t* data,
    size_t size) {
    if(!session->prog_mode) {
        snprintf(session->error, sizeof(session->error), "Not in prog mode");
        return false;
    }
    return updi_nvm_read_flash(&session->link, byte_address, data, size);
}

bool updi_session_read_fuses(UpdiSession* session, uint8_t fuses[12]) {
    if(!session->prog_mode) {
        snprintf(session->error, sizeof(session->error), "Not in prog mode");
        return false;
    }
    return updi_link_read(&session->link, UPDI_FUSE_BASE, fuses, UPDI_FUSE_SIZE);
}

void updi_session_disconnect(UpdiSession* session) {
    if(session == NULL || !session->open) return;

    char saved[sizeof(session->error)];
    snprintf(saved, sizeof(saved), "%s", session->error);

    if(session->prog_mode) {
        updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, UPDI_RESET_REQ_VALUE);
        updi_link_stcs(&session->link, UPDI_ASI_RESET_REQ, 0x00);
        session->prog_mode = false;
    }
    updi_link_stcs(
        &session->link, UPDI_CS_CTRLB, (uint8_t)(UPDI_CTRLB_UPDIDIS | UPDI_CTRLB_CCDETDIS));

    updi_transport_close(session->transport);
    session->open = false;
    snprintf(session->error, sizeof(session->error), "%s", saved);
}

const char* updi_session_error(const UpdiSession* session) {
    return session->error;
}

const UpdiTargetInfo* updi_session_target(const UpdiSession* session) {
    return &session->target;
}

uint32_t updi_session_baud(const UpdiSession* session) {
    return updi_transport_baud(session->transport);
}

bool updi_session_in_progmode(const UpdiSession* session) {
    return session->prog_mode;
}
