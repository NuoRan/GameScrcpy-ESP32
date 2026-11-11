#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "services/hid/ble_svc_hid.h"

#include "gs_ble.h"
#include "gs_hid.h"
#include "gs_hid_descriptors.h"
#include "gs_hid_reports.h"

#define GS_BLE_VID 0x303A
#define GS_BLE_PID 0x40A5
#define GS_BLE_VERSION 0x0100
#define GS_BLE_RETRIES 10
#define GS_BLE_REPORTS 8

static const char *gs_tag = "gs_ble";
static esp_hidd_dev_t *gs_device;
static ble_gatt_register_fn *gs_hidd_register;
static uint16_t gs_inputs[GS_BLE_REPORTS];
static char gs_name[GS_ESP32_MAX_NAME + 1];
static char gs_serial[13];

static esp_hid_raw_report_map_t gs_report_maps[] = {
    {.data = gs_combined_descriptor, .len = sizeof(gs_combined_descriptor)},
};

static esp_hid_device_config_t gs_config = {
    .vendor_id = GS_BLE_VID,
    .product_id = GS_BLE_PID,
    .version = GS_BLE_VERSION,
    .device_name = gs_name,
    .manufacturer_name = "GameScrcpy",
    .serial_number = gs_serial,
    .report_maps = gs_report_maps,
    .report_maps_len = 1,
};

static void gs_copy(char *out, size_t capacity, const char *text)
{
    size_t length = 0;
    while (length + 1 < capacity && text[length] != '\0') {
        ++length;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static void gs_register(struct ble_gatt_register_ctxt *context, void *argument)
{
    struct os_mbuf *om = NULL;
    uint16_t reference = 0;
    int copied = -1;
    if (gs_hidd_register != NULL) {
        gs_hidd_register(context, argument);
    }
    if (context->op != BLE_GATT_REGISTER_OP_DSC || context->dsc.dsc_def->arg == NULL ||
        ble_uuid_u16(context->dsc.dsc_def->uuid) != BLE_SVC_HID_DSC_UUID16_RPT_REF) {
        return;
    }
    if (ble_att_svr_read_local(context->dsc.handle, &om) != 0) {
        return;
    }
    copied = ble_hs_mbuf_to_flat(om, &reference, sizeof(reference), NULL);
    os_mbuf_free_chain(om);
    const uint8_t id = (uint8_t)(reference & 0xFFu);
    const uint8_t type = (uint8_t)(reference >> 8);
    if (copied == 0 && type == ESP_HID_REPORT_TYPE_INPUT && id < GS_BLE_REPORTS) {
        gs_inputs[id] = *(const uint16_t *)context->dsc.dsc_def->arg;
    }
}

bool gs_hid_start(const char *name, const uint8_t mac[6])
{
    uint8_t limit = GS_ESP32_MAX_CONTACTS;
    esp_err_t result;
    gs_copy(gs_name, sizeof(gs_name), name);
    snprintf(gs_serial, sizeof(gs_serial), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!gs_ble_prepare(name, mac)) {
        return false;
    }
    result = esp_hidd_dev_init(&gs_config, ESP_HID_TRANSPORT_BLE, NULL, &gs_device);
    if (result != ESP_OK) {
        ESP_LOGE(gs_tag, "the Bluetooth HID device could not start (%s); the board keeps running without it",
                 esp_err_to_name(result));
        return false;
    }
    gs_hidd_register = ble_hs_cfg.gatts_register_cb;
    ble_hs_cfg.gatts_register_cb = gs_register;
    esp_hidd_dev_feature_set(gs_device, 0, GS_REPORT_TOUCH_LIMITS, &limit, 1);
    if (!gs_ble_start()) {
        return false;
    }
    ESP_LOGI(gs_tag, "Bluetooth touch screen, keyboard, mouse and gamepad ready");
    return true;
}

static bool gs_send(uint8_t report_id, uint8_t *report, size_t length, bool important)
{
    const int attempts = important ? GS_BLE_RETRIES : 1;
    const uint16_t handle = report_id < GS_BLE_REPORTS ? gs_inputs[report_id] : 0;
    if (gs_device == NULL || handle == 0) {
        return false;
    }
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const uint16_t phone = gs_ble_phone();
        if (phone == GS_BLE_NO_CONNECTION) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(report, (uint16_t)length);
        if (om != NULL && ble_gatts_notify_custom(phone, handle, om) == 0) {
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

bool gs_hid_touch(const gs_esp32_touch *touch)
{
    uint8_t report[GS_TOUCH_REPORT_SIZE];
    const size_t length = gs_report_touch(touch, report);
    return gs_send(GS_REPORT_TOUCH, report, length, gs_report_lifts(touch));
}

bool gs_hid_keyboard(const gs_esp32_keyboard *keyboard)
{
    uint8_t report[GS_KEYBOARD_REPORT_SIZE];
    const size_t length = gs_report_keyboard(keyboard, report);
    return gs_send(GS_REPORT_KEYBOARD, report, length, true);
}

bool gs_hid_consumer(uint16_t usage)
{
    uint8_t report[GS_CONSUMER_REPORT_SIZE];
    const size_t length = gs_report_consumer(usage, report);
    return gs_send(GS_REPORT_CONSUMER, report, length, true);
}

bool gs_hid_mouse(const gs_esp32_mouse *mouse)
{
    uint8_t report[GS_MOUSE_REPORT_SIZE];
    const size_t length = gs_report_mouse(mouse, report);
    return gs_send(GS_REPORT_MOUSE, report, length, mouse->dx == 0 && mouse->dy == 0);
}

bool gs_hid_gamepad(const gs_esp32_gamepad *gamepad)
{
    uint8_t report[GS_GAMEPAD_REPORT_SIZE];
    const size_t length = gs_report_gamepad(gamepad, report);
    return gs_send(GS_REPORT_GAMEPAD, report, length, true);
}

void gs_hid_phone(gs_esp32_status *status)
{
    status->usb = GS_ESP32_USB_NONE;
    gs_ble_phone_status(status);
}

uint32_t gs_hid_features(void)
{
    return GS_ESP32_FEATURE_BLE_HID;
}

void gs_hid_rename(const char *name)
{
    gs_copy(gs_name, sizeof(gs_name), name);
}
