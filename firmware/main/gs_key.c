#include "gs_key.h"

#include <string.h>

#include "nvs.h"

#define GS_KEY_NAMESPACE "gs"
#define GS_KEY_NAME "key"

bool gs_key_load(uint8_t key[GS_ESP32_KEY_SIZE])
{
    nvs_handle_t handle;
    size_t length = GS_ESP32_KEY_SIZE;
    bool loaded = false;
    if (nvs_open(GS_KEY_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    loaded = nvs_get_blob(handle, GS_KEY_NAME, key, &length) == ESP_OK && length == GS_ESP32_KEY_SIZE;
    nvs_close(handle);
    return loaded;
}

bool gs_key_store(const uint8_t key[GS_ESP32_KEY_SIZE])
{
    nvs_handle_t handle;
    bool stored = false;
    if (nvs_open(GS_KEY_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    stored = nvs_set_blob(handle, GS_KEY_NAME, key, GS_ESP32_KEY_SIZE) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return stored;
}

bool gs_key_present(void)
{
    uint8_t key[GS_ESP32_KEY_SIZE];
    const bool present = gs_key_load(key);
    memset(key, 0, sizeof(key));
    return present;
}
