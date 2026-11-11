#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gs_esp32_protocol.h"

#define GS_BLE_NO_CONNECTION 0xFFFF

bool gs_ble_prepare(const char *name, const uint8_t mac[6]);
bool gs_ble_start(void);
bool gs_ble_link_start(const char *name, const uint8_t mac[6]);
bool gs_ble_link_available(void);
void gs_ble_link_send(const uint8_t *frame, size_t size);
void gs_ble_set_key(const uint8_t key[GS_ESP32_KEY_SIZE]);
void gs_ble_forget_key(void);
void gs_ble_rename(const char *name);
bool gs_ble_signal(int8_t *signal);
uint16_t gs_ble_phone(void);
void gs_ble_phone_status(gs_esp32_status *status);
