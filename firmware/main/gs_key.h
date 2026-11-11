#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gs_esp32_protocol.h"

bool gs_key_load(uint8_t key[GS_ESP32_KEY_SIZE]);
bool gs_key_store(const uint8_t key[GS_ESP32_KEY_SIZE]);
bool gs_key_present(void);
