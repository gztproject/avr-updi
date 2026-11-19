#include "updi_device.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char* name;
    uint8_t signature[3];
} UpdiDevice;

/* SIGROW.DEVICEID from the AVR128DA datasheet, Table 7-4. */
static const UpdiDevice updi_devices[] = {
    {"AVR128DA64", {0x1E, 0x97, 0x07}},
    {"AVR128DA48", {0x1E, 0x97, 0x08}},
    {"AVR128DA32", {0x1E, 0x97, 0x09}},
    {"AVR128DA28", {0x1E, 0x97, 0x0A}},
    {"AVR128DA64S", {0x1E, 0x97, 0x0F}},
    {"AVR128DA48S", {0x1E, 0x97, 0x10}},
    {"AVR128DA32S", {0x1E, 0x97, 0x11}},
    {"AVR128DA28S", {0x1E, 0x97, 0x12}},
};

const char* updi_device_name(const uint8_t signature[3]) {
    for(size_t i = 0; i < sizeof(updi_devices) / sizeof(updi_devices[0]); i++) {
        if(memcmp(updi_devices[i].signature, signature, 3) == 0) return updi_devices[i].name;
    }
    return NULL;
}

void updi_target_set_signature(UpdiTargetInfo* info, const uint8_t signature[3]) {
    memcpy(info->signature, signature, 3);
    const char* name = updi_device_name(signature);
    info->known = name != NULL;
    if(name != NULL) {
        snprintf(info->name, sizeof(info->name), "%s", name);
    } else {
        snprintf(info->name, sizeof(info->name), "Unknown");
    }
}
