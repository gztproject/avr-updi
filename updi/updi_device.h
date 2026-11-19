#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDI_DEVICE_NAME_LEN 16

typedef struct {
    char name[UPDI_DEVICE_NAME_LEN];
    uint8_t signature[3];
    bool known;
    bool locked;
    char nvm[4];
    char sib[33];
} UpdiTargetInfo;

const char* updi_device_name(const uint8_t signature[3]);
void updi_target_set_signature(UpdiTargetInfo* info, const uint8_t signature[3]);
