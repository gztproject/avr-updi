#include "updi_app_i.h"

#include "hex/intel_hex.h"
#include "updi/updi_constants.h"

#include <storage/storage.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char* fuse_names[UPDI_FUSE_SIZE] = {
    "WDTCFG",
    "BODCFG",
    "OSCCFG",
    "Reserved",
    "Reserved",
    "SYSCFG0",
    "SYSCFG1",
    "CODESIZE",
    "BOOTSIZE",
    "Reserved",
    "PDICFG L",
    "PDICFG H",
};

static void updi_result(UpdiApp* app, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->result, sizeof(app->result), fmt, args);
    va_end(args);
}

static bool updi_report(UpdiApp* app, const char* status, uint8_t percent) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->status, sizeof(app->status), "%s", status);
    app->percent = percent;
    furi_mutex_release(app->mutex);
    view_dispatcher_send_custom_event(app->view_dispatcher, UpdiEventProgress);
    return !app->cancel;
}

static void updi_finish(UpdiApp* app) {
    view_dispatcher_send_custom_event(app->view_dispatcher, UpdiEventFinished);
}

static bool updi_require_known(UpdiApp* app, const UpdiTargetInfo* target, bool erased) {
    if(target->known) return true;
    if(erased) {
        updi_result(
            app,
            "Chip was erased, but signature %02X %02X %02X is not an AVR128DA. Nothing was written.",
            target->signature[0],
            target->signature[1],
            target->signature[2]);
    } else {
        updi_result(
            app,
            "%s\nSig %02X %02X %02X\nNot an AVR128DA.\nFlash and dump are refused.",
            target->name,
            target->signature[0],
            target->signature[1],
            target->signature[2]);
    }
    return false;
}

typedef enum {
    HexPassCount,
    HexPassWrite,
    HexPassVerify,
} HexPass;

typedef struct {
    UpdiApp* app;
    UpdiSession* session;
    HexPass pass;
    uint32_t pages;
    bool failed;
    bool has_mismatch;
    uint32_t mismatch;
    uint8_t file_byte;
    uint8_t chip_byte;
} HexJob;

static bool hex_on_page(uint32_t address, const uint8_t data[INTEL_HEX_PAGE_SIZE], void* context) {
    HexJob* job = context;
    if(job->app->cancel) return false;

    if(job->pass == HexPassCount) {
        job->pages++;
        return true;
    }

    if(job->pass == HexPassWrite) {
        if(!updi_session_write_page(job->session, address, data)) {
            job->failed = true;
            return false;
        }
        job->pages++;
        return true;
    }

    uint8_t chip[INTEL_HEX_PAGE_SIZE];
    if(!updi_session_read_flash(job->session, address, chip, sizeof(chip))) {
        job->failed = true;
        return false;
    }
    for(size_t i = 0; i < INTEL_HEX_PAGE_SIZE; i++) {
        if(chip[i] != data[i]) {
            job->failed = true;
            job->has_mismatch = true;
            job->mismatch = address + (uint32_t)i;
            job->file_byte = data[i];
            job->chip_byte = chip[i];
            return false;
        }
    }
    job->pages++;
    return true;
}

static bool hex_pass(
    UpdiApp* app,
    File* file,
    uint64_t file_size,
    HexJob* job,
    HexPass pass,
    uint8_t percent_from,
    uint8_t percent_to,
    char* parse_error,
    size_t parse_error_size) {
    const char* label = "Checking HEX";
    if(pass == HexPassWrite) label = "Writing flash";
    if(pass == HexPassVerify) label = "Verifying";

    if(!storage_file_seek(file, 0, true)) {
        snprintf(parse_error, parse_error_size, "Could not rewind HEX file");
        return false;
    }

    job->pass = pass;
    job->pages = 0;
    job->failed = false;

    IntelHexParser* parser =
        intel_hex_parser_alloc(UPDI_FLASH_SIZE, hex_on_page, job);
    if(parser == NULL) {
        snprintf(parse_error, parse_error_size, "Out of memory");
        return false;
    }

    char line[600];
    size_t line_len = 0;
    uint8_t chunk[128];
    uint64_t pos = 0;
    bool ok = true;

    while(ok) {
        size_t nread = storage_file_read(file, chunk, sizeof(chunk));
        pos += nread;
        for(size_t i = 0; i < nread && ok; i++) {
            char c = (char)chunk[i];
            if(c == '\n' || c == '\r') {
                if(line_len == 0) continue;
                line[line_len] = '\0';
                line_len = 0;
                if(!intel_hex_parser_feed(parser, line, parse_error, parse_error_size)) ok = false;
            } else if(line_len + 1 < sizeof(line)) {
                line[line_len++] = c;
            } else {
                snprintf(parse_error, parse_error_size, "HEX line too long");
                ok = false;
            }
        }

        uint8_t percent = percent_from;
        if(file_size > 0) {
            percent = (uint8_t)(percent_from + ((percent_to - percent_from) * pos) / file_size);
        }
        if(!updi_report(app, label, percent)) {
            snprintf(parse_error, parse_error_size, "Cancelled");
            ok = false;
        }
        if(nread == 0) break;
    }

    if(ok && line_len > 0) {
        line[line_len] = '\0';
        if(!intel_hex_parser_feed(parser, line, parse_error, parse_error_size)) ok = false;
    }
    if(ok && !intel_hex_parser_finish(parser, parse_error, parse_error_size)) ok = false;

    intel_hex_parser_free(parser);
    return ok;
}

static bool dump_write_line(const char* line, void* context) {
    File* file = context;
    size_t len = strlen(line);
    if(storage_file_write(file, line, len) != len) return false;
    return storage_file_write(file, "\n", 1) == 1;
}

static void updi_worker_read(UpdiApp* app, UpdiSession* session) {
    if(!updi_report(app, "Connecting", 0)) {
        updi_result(app, "Cancelled");
        return;
    }

    UpdiConnectStatus status = updi_session_connect(session);
    const UpdiTargetInfo* target = updi_session_target(session);
    if(status == UpdiConnectFail) {
        updi_result(app, "%s", updi_session_error(session));
        return;
    }
    if(status == UpdiConnectLocked) {
        updi_result(
            app,
            "Device is locked.\nNVM %s\nSIB: %s\n\nFlash chip-erases to unlock. Fuses stay hidden until then.",
            target->nvm,
            target->sib);
        return;
    }

    updi_result(
        app,
        "%s\nSig %02X %02X %02X\nNVM %s\nBaud %lu\nSIB: %s",
        target->name,
        target->signature[0],
        target->signature[1],
        target->signature[2],
        target->nvm,
        (unsigned long)updi_session_baud(session),
        target->sib);
}

static void updi_worker_fuses(UpdiApp* app, UpdiSession* session) {
    if(!updi_report(app, "Connecting", 0)) {
        updi_result(app, "Cancelled");
        return;
    }
    UpdiConnectStatus status = updi_session_connect(session);
    const UpdiTargetInfo* target = updi_session_target(session);
    if(status == UpdiConnectFail) {
        updi_result(app, "%s", updi_session_error(session));
        return;
    }
    if(status == UpdiConnectLocked) {
        updi_result(app, "Device is locked.\nFuses are hidden until a chip erase.");
        return;
    }

    uint8_t fuses[UPDI_FUSE_SIZE];
    if(!updi_session_read_fuses(session, fuses)) {
        updi_result(app, "%s", updi_session_error(session));
        return;
    }

    char body[512];
    size_t used = 0;
    used += (size_t)snprintf(
        body,
        sizeof(body),
        "%s\nSig %02X %02X %02X\n",
        target->name,
        target->signature[0],
        target->signature[1],
        target->signature[2]);
    for(size_t i = 0; i < UPDI_FUSE_SIZE && used < sizeof(body); i++) {
        used += (size_t)snprintf(
            body + used, sizeof(body) - used, "%s %02X\n", fuse_names[i], fuses[i]);
    }
    updi_result(app, "%s", body);
}

static void updi_worker_dump(UpdiApp* app, UpdiSession* session) {
    if(!updi_report(app, "Connecting", 0)) {
        updi_result(app, "Cancelled");
        return;
    }
    UpdiConnectStatus status = updi_session_connect(session);
    if(status == UpdiConnectFail) {
        updi_result(app, "%s", updi_session_error(session));
        return;
    }
    if(status == UpdiConnectLocked) {
        updi_result(app, "Device is locked.\nDump needs a chip erase, which Flash can do.");
        return;
    }
    if(!updi_require_known(app, updi_session_target(session), false)) return;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, "/ext/apps_data/updi_avr128");
    File* file = storage_file_alloc(storage);
    bool opened =
        storage_file_open(file, UPDI_DUMP_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(!opened) {
        updi_result(app, "Could not create\n%s", UPDI_DUMP_PATH);
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    IntelHexWriter* writer = intel_hex_writer_alloc(dump_write_line, file);
    char error[96] = {0};
    bool ok = writer != NULL;
    if(!ok) snprintf(error, sizeof(error), "Out of memory");

    uint8_t buffer[256];
    for(uint32_t address = 0; ok && address < UPDI_FLASH_SIZE; address += sizeof(buffer)) {
        if(app->cancel) {
            snprintf(error, sizeof(error), "Cancelled");
            ok = false;
            break;
        }
        if(!updi_session_read_flash(session, address, buffer, sizeof(buffer))) {
            snprintf(error, sizeof(error), "%s", updi_session_error(session));
            ok = false;
            break;
        }
        if(!intel_hex_writer_feed(writer, address, buffer, sizeof(buffer), error, sizeof(error))) {
            ok = false;
            break;
        }
        uint8_t percent = (uint8_t)((address * 100u) / UPDI_FLASH_SIZE);
        if(!updi_report(app, "Dumping flash", percent)) {
            snprintf(error, sizeof(error), "Cancelled");
            ok = false;
            break;
        }
    }
    if(ok && !intel_hex_writer_finish(writer, error, sizeof(error))) ok = false;

    intel_hex_writer_free(writer);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(ok) {
        updi_result(app, "Dumped 128KB to\n%s", UPDI_DUMP_PATH);
    } else {
        updi_result(app, "%s", error[0] ? error : "Dump failed");
    }
}

static void updi_worker_flash(UpdiApp* app, UpdiSession* session) {
    if(!updi_report(app, "Connecting", 0)) {
        updi_result(app, "Cancelled");
        return;
    }

    bool erased_by_key = false;
    UpdiConnectStatus status = updi_session_connect(session);
    if(status == UpdiConnectFail) {
        updi_result(app, "%s", updi_session_error(session));
        return;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    const char* path = furi_string_get_cstr(app->hex_path);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        updi_result(app, "Could not open HEX file");
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    uint64_t file_size = storage_file_size(file);
    HexJob* job = calloc(1, sizeof(HexJob));
    char parse_error[96] = {0};
    bool ok = job != NULL;
    if(!ok) snprintf(parse_error, sizeof(parse_error), "Out of memory");

    if(ok) {
        job->app = app;
        job->session = session;
        ok = hex_pass(
            app, file, file_size, job, HexPassCount, 8, 25, parse_error, sizeof(parse_error));
    }
    uint32_t pages = ok ? job->pages : 0;
    if(ok && pages == 0) {
        snprintf(parse_error, sizeof(parse_error), "HEX file has no flash data");
        ok = false;
    }

    /* Validate the file before a locked-device erase key, which cannot be undone. */
    if(ok && status == UpdiConnectLocked) {
        if(!updi_report(app, "Erasing lock", 28)) {
            snprintf(parse_error, sizeof(parse_error), "Cancelled");
            ok = false;
        } else if(
            !updi_session_unlock(session) || !updi_session_enter_progmode(session) ||
            !updi_session_read_signature(session)) {
            snprintf(parse_error, sizeof(parse_error), "%s", updi_session_error(session));
            ok = false;
        } else {
            erased_by_key = true;
        }
    }

    const UpdiTargetInfo* target = updi_session_target(session);
    if(ok && !target->known) {
        if(erased_by_key) {
            snprintf(
                parse_error,
                sizeof(parse_error),
                "Chip was erased, but signature %02X %02X %02X is not an AVR128DA. Nothing was written.",
                target->signature[0],
                target->signature[1],
                target->signature[2]);
        } else {
            snprintf(
                parse_error,
                sizeof(parse_error),
                "Sig %02X %02X %02X is not an AVR128DA. Nothing was erased.",
                target->signature[0],
                target->signature[1],
                target->signature[2]);
        }
        ok = false;
    }

    if(ok) {
        if(!updi_report(app, "Erasing chip", 30)) {
            snprintf(parse_error, sizeof(parse_error), "Cancelled");
            ok = false;
        }
    }
    if(ok && !updi_session_chip_erase(session)) {
        snprintf(parse_error, sizeof(parse_error), "%s", updi_session_error(session));
        ok = false;
    }

    if(ok) {
        ok = hex_pass(
            app, file, file_size, job, HexPassWrite, 40, 75, parse_error, sizeof(parse_error));
        if(!ok && job->failed) {
            snprintf(parse_error, sizeof(parse_error), "%s", updi_session_error(session));
        }
    }

    if(ok) {
        ok = hex_pass(
            app, file, file_size, job, HexPassVerify, 75, 100, parse_error, sizeof(parse_error));
        if(!ok && job != NULL && job->has_mismatch) {
            snprintf(
                parse_error,
                sizeof(parse_error),
                "Verify mismatch at 0x%05lX file %02X chip %02X",
                (unsigned long)job->mismatch,
                job->file_byte,
                job->chip_byte);
        } else if(!ok && job != NULL && job->failed) {
            snprintf(parse_error, sizeof(parse_error), "%s", updi_session_error(session));
        }
    }

    if(ok) {
        updi_result(
            app,
            "Flashed %s\nPages: %lu\nVerified\nBaud %lu",
            target->name,
            (unsigned long)pages,
            (unsigned long)updi_session_baud(session));
    } else if(app->cancel) {
        updi_result(app, "Cancelled");
    } else {
        updi_result(app, "%s", parse_error[0] ? parse_error : "Flash failed");
    }

    free(job);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

int32_t updi_worker(void* context) {
    UpdiApp* app = context;
    UpdiSession* session = updi_session_alloc();
    if(session == NULL) {
        updi_result(app, "Out of memory");
        updi_finish(app);
        return 0;
    }
    updi_session_set_pins(session, app->pins);

    switch(app->op) {
    case UpdiOpRead:
        updi_worker_read(app, session);
        break;
    case UpdiOpFlash:
        updi_worker_flash(app, session);
        break;
    case UpdiOpDump:
        updi_worker_dump(app, session);
        break;
    case UpdiOpFuses:
        updi_worker_fuses(app, session);
        break;
    }

    updi_session_disconnect(session);
    updi_session_free(session);
    updi_report(app, "Done", 100);
    updi_finish(app);
    return 0;
}
