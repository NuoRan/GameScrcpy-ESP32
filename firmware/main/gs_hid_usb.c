#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"

#include "gs_hid.h"
#include "gs_hid_descriptors.h"
#include "gs_hid_reports.h"
#include "gs_hid_sched.h"

#define GS_USB_VID 0x303A
#define GS_USB_PID 0x40A5
#define GS_USB_RELEASE 0x0100
#define GS_USB_POWER_MA 100
#define GS_USB_INTERVAL 1
#define GS_USB_ENDPOINT_SIZE 64
#define GS_USB_DROP_LOG_US 1000000

enum {
    GS_ITF_TOUCH = GS_HID_INTERFACE_TOUCH,
    GS_ITF_KEYBOARD = GS_HID_INTERFACE_KEYBOARD,
    GS_ITF_MOUSE = GS_HID_INTERFACE_MOUSE,
    GS_ITF_GAMEPAD = GS_HID_INTERFACE_GAMEPAD,
    GS_ITF_COUNT = GS_HID_INTERFACE_COUNT,
};

#define GS_EP_TOUCH 0x81
#define GS_EP_KEYBOARD 0x82
#define GS_EP_MOUSE 0x83
#define GS_EP_GAMEPAD 0x84
#define GS_CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + GS_ITF_COUNT * TUD_HID_DESC_LEN)

static const char *gs_tag = "gs_usb";
static TaskHandle_t gs_sender;
static char gs_serial[13];

typedef struct {
    uint8_t report_id;
    uint8_t length;
    uint8_t data[GS_KEYBOARD_REPORT_SIZE];
} gs_usb_keyboard_report;

static gs_esp32_touch gs_touch_items[GS_HID_TOUCH_STATES];
static gs_usb_keyboard_report gs_keyboard_items[GS_HID_KEYBOARD_STATES];
static gs_esp32_gamepad gs_gamepad_items[GS_HID_GAMEPAD_STATES];
static gs_hid_state_queue gs_touch_queue;
static gs_hid_state_queue gs_keyboard_queue;
static gs_hid_state_queue gs_gamepad_queue;
static gs_hid_mouse_merge gs_mouse;
static SemaphoreHandle_t gs_state_lock;
static int64_t gs_drop_logged;

static const tusb_desc_device_t gs_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = GS_USB_VID,
    .idProduct = GS_USB_PID,
    .bcdDevice = GS_USB_RELEASE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *gs_strings[] = {
    (const char[]){0x09, 0x04},
    "GameScrcpy",
    "GameScrcpy Touch",
    gs_serial,
    "GameScrcpy Touch Screen",
    "GameScrcpy Keyboard",
    "GameScrcpy Mouse",
    "GameScrcpy Gamepad",
};

static const uint8_t gs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, GS_ITF_COUNT, 0, GS_CONFIG_LENGTH, 0, GS_USB_POWER_MA),
    TUD_HID_DESCRIPTOR(GS_ITF_TOUCH, 4, HID_ITF_PROTOCOL_NONE, sizeof(gs_touch_descriptor), GS_EP_TOUCH,
                       GS_USB_ENDPOINT_SIZE, GS_USB_INTERVAL),
    TUD_HID_DESCRIPTOR(GS_ITF_KEYBOARD, 5, HID_ITF_PROTOCOL_NONE, sizeof(gs_keyboard_descriptor), GS_EP_KEYBOARD,
                       GS_USB_ENDPOINT_SIZE, GS_USB_INTERVAL),
    TUD_HID_DESCRIPTOR(GS_ITF_MOUSE, 6, HID_ITF_PROTOCOL_NONE, sizeof(gs_mouse_descriptor), GS_EP_MOUSE,
                       GS_USB_ENDPOINT_SIZE, GS_USB_INTERVAL),
    TUD_HID_DESCRIPTOR(GS_ITF_GAMEPAD, 7, HID_ITF_PROTOCOL_NONE, sizeof(gs_gamepad_descriptor), GS_EP_GAMEPAD,
                       GS_USB_ENDPOINT_SIZE, GS_USB_INTERVAL),
};

#if TUD_OPT_HIGH_SPEED
static const tusb_desc_device_qualifier_t gs_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00,
};
#endif

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case GS_ITF_TOUCH:
        return gs_touch_descriptor;
    case GS_ITF_KEYBOARD:
        return gs_keyboard_descriptor;
    case GS_ITF_MOUSE:
        return gs_mouse_descriptor;
    default:
        return gs_gamepad_descriptor;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    if (instance == GS_ITF_TOUCH && report_id == GS_REPORT_TOUCH_LIMITS && report_type == HID_REPORT_TYPE_FEATURE &&
        reqlen >= 1) {
        buffer[0] = GS_ESP32_MAX_CONTACTS;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)instance;
    (void)report;
    (void)len;
    if (gs_sender != NULL) {
        xTaskNotifyGive(gs_sender);
    }
}

static void gs_clear_all(void)
{
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    gs_hid_state_queue_clear(&gs_touch_queue);
    gs_hid_state_queue_clear(&gs_keyboard_queue);
    gs_hid_state_queue_clear(&gs_gamepad_queue);
    gs_hid_mouse_merge_clear(&gs_mouse);
    xSemaphoreGive(gs_state_lock);
}

static void gs_note_drops(void)
{
    const int64_t now = esp_timer_get_time();
    uint32_t touch = 0;
    uint32_t keyboard = 0;
    uint32_t mouse = 0;
    uint32_t gamepad = 0;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    if (now - gs_drop_logged < (int64_t)GS_USB_DROP_LOG_US) {
        xSemaphoreGive(gs_state_lock);
        return;
    }
    gs_drop_logged = now;
    touch = gs_hid_state_queue_dropped(&gs_touch_queue);
    keyboard = gs_hid_state_queue_dropped(&gs_keyboard_queue);
    mouse = gs_hid_mouse_merge_dropped(&gs_mouse);
    gamepad = gs_hid_state_queue_dropped(&gs_gamepad_queue);
    xSemaphoreGive(gs_state_lock);
    ESP_LOGW(gs_tag, "USB reports dropped so far: touch %u, keyboard %u, mouse %u, gamepad %u", (unsigned)touch,
             (unsigned)keyboard, (unsigned)mouse, (unsigned)gamepad);
}

static bool gs_accepting(void)
{
    if (gs_state_lock == NULL || gs_sender == NULL) {
        return false;
    }
    if (!tud_mounted()) {
        gs_clear_all();
        return false;
    }
    return true;
}

static uint8_t gs_pending(void)
{
    uint8_t mask = 0;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    if (gs_hid_state_queue_pending(&gs_touch_queue)) {
        mask |= (uint8_t)(1u << GS_ITF_TOUCH);
    }
    if (gs_hid_state_queue_pending(&gs_keyboard_queue)) {
        mask |= (uint8_t)(1u << GS_ITF_KEYBOARD);
    }
    if (gs_hid_mouse_merge_pending(&gs_mouse)) {
        mask |= (uint8_t)(1u << GS_ITF_MOUSE);
    }
    if (gs_hid_state_queue_pending(&gs_gamepad_queue)) {
        mask |= (uint8_t)(1u << GS_ITF_GAMEPAD);
    }
    xSemaphoreGive(gs_state_lock);
    return mask;
}

static uint8_t gs_ready(void)
{
    uint8_t mask = 0;
    uint8_t instance;
    for (instance = 0; instance < GS_ITF_COUNT; ++instance) {
        if (tud_hid_n_ready(instance)) {
            mask |= (uint8_t)(1u << instance);
        }
    }
    return mask;
}

static bool gs_send_touch(void)
{
    const gs_esp32_touch *head;
    gs_esp32_touch state;
    uint8_t report[GS_TOUCH_REPORT_SIZE];
    bool sent = false;
    size_t length;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    head = (const gs_esp32_touch *)gs_hid_state_queue_head(&gs_touch_queue);
    if (head != NULL) {
        memcpy(&state, head, sizeof(state));
        length = gs_report_touch(&state, report);
        sent = tud_hid_n_report(GS_ITF_TOUCH, GS_REPORT_TOUCH, report, (uint16_t)length);
        if (sent) {
            gs_hid_state_queue_pop(&gs_touch_queue);
        }
    }
    xSemaphoreGive(gs_state_lock);
    return sent;
}

static bool gs_send_keyboard(void)
{
    const gs_usb_keyboard_report *head;
    gs_usb_keyboard_report item;
    bool sent = false;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    head = (const gs_usb_keyboard_report *)gs_hid_state_queue_head(&gs_keyboard_queue);
    if (head != NULL) {
        memcpy(&item, head, sizeof(item));
        sent = tud_hid_n_report(GS_ITF_KEYBOARD, item.report_id, item.data, item.length);
        if (sent) {
            gs_hid_state_queue_pop(&gs_keyboard_queue);
        }
    }
    xSemaphoreGive(gs_state_lock);
    return sent;
}

static bool gs_send_mouse(void)
{
    gs_esp32_mouse chunk;
    uint8_t report[GS_MOUSE_REPORT_SIZE];
    bool sent = false;
    size_t length;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    if (gs_hid_mouse_merge_peek(&gs_mouse, GS_HID_AXIS_MAX, GS_HID_WHEEL_MAX, &chunk)) {
        length = gs_report_mouse(&chunk, report);
        sent = tud_hid_n_report(GS_ITF_MOUSE, GS_REPORT_MOUSE, report, (uint16_t)length);
        if (sent) {
            gs_hid_mouse_merge_commit(&gs_mouse, &chunk);
        }
    }
    xSemaphoreGive(gs_state_lock);
    return sent;
}

static bool gs_send_gamepad(void)
{
    const gs_esp32_gamepad *head;
    gs_esp32_gamepad state;
    uint8_t report[GS_GAMEPAD_REPORT_SIZE];
    bool sent = false;
    size_t length;
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    head = (const gs_esp32_gamepad *)gs_hid_state_queue_head(&gs_gamepad_queue);
    if (head != NULL) {
        memcpy(&state, head, sizeof(state));
        length = gs_report_gamepad(&state, report);
        sent = tud_hid_n_report(GS_ITF_GAMEPAD, GS_REPORT_GAMEPAD, report, (uint16_t)length);
        if (sent) {
            gs_hid_state_queue_pop(&gs_gamepad_queue);
        }
    }
    xSemaphoreGive(gs_state_lock);
    return sent;
}

static void gs_send_task(void *argument)
{
    uint8_t cursor = GS_ITF_COUNT - 1;
    (void)argument;
    for (;;) {
        uint8_t pending;
        uint8_t next;
        if (!tud_mounted()) {
            gs_clear_all();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        pending = gs_pending();
        next = pending != 0 ? gs_hid_pick_interface(pending, gs_ready(), cursor) : GS_HID_INTERFACE_NONE;
        if (next == GS_HID_INTERFACE_NONE) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        cursor = next;
        switch (next) {
        case GS_ITF_TOUCH:
            gs_send_touch();
            break;
        case GS_ITF_KEYBOARD:
            gs_send_keyboard();
            break;
        case GS_ITF_MOUSE:
            gs_send_mouse();
            break;
        default:
            gs_send_gamepad();
            break;
        }
    }
}

bool gs_hid_start(const char *name, const uint8_t mac[6])
{
    (void)name;
    snprintf(gs_serial, sizeof(gs_serial), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    gs_state_lock = xSemaphoreCreateMutex();
    if (gs_state_lock == NULL) {
        ESP_LOGE(gs_tag, "could not make the USB state lock; the board keeps running without USB");
        return false;
    }
    gs_hid_state_queue_init(&gs_touch_queue, gs_touch_items, (uint8_t)GS_HID_TOUCH_STATES,
                            (uint8_t)sizeof(gs_esp32_touch));
    gs_hid_state_queue_init(&gs_keyboard_queue, gs_keyboard_items, (uint8_t)GS_HID_KEYBOARD_STATES,
                            (uint8_t)sizeof(gs_usb_keyboard_report));
    gs_hid_state_queue_init(&gs_gamepad_queue, gs_gamepad_items, (uint8_t)GS_HID_GAMEPAD_STATES,
                            (uint8_t)sizeof(gs_esp32_gamepad));
    gs_hid_mouse_merge_init(&gs_mouse);
    gs_drop_logged = 0;
    if (xTaskCreate(gs_send_task, "gs_usb_send", 4096, NULL, configMAX_PRIORITIES - 2, &gs_sender) != pdPASS) {
        ESP_LOGE(gs_tag, "could not start the USB sender; the board keeps running without USB");
        return false;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.descriptor.device = &gs_device;
    config.descriptor.string = gs_strings;
    config.descriptor.string_count = sizeof(gs_strings) / sizeof(gs_strings[0]);
    config.descriptor.full_speed_config = gs_configuration;
#if TUD_OPT_HIGH_SPEED
    config.descriptor.high_speed_config = gs_configuration;
    config.descriptor.qualifier = &gs_qualifier;
#endif
    if (tinyusb_driver_install(&config) != ESP_OK) {
        ESP_LOGE(gs_tag, "USB could not start; the board keeps running without it");
        return false;
    }
    ESP_LOGI(gs_tag, "USB touch screen, keyboard, mouse and gamepad ready");
    return true;
}

bool gs_hid_touch(const gs_esp32_touch *touch)
{
    int dropped;
    if (!gs_accepting()) {
        return false;
    }
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    dropped = gs_hid_state_queue_push(&gs_touch_queue, touch);
    xSemaphoreGive(gs_state_lock);
    xTaskNotifyGive(gs_sender);
    if (dropped) {
        gs_note_drops();
    }
    return true;
}

bool gs_hid_keyboard(const gs_esp32_keyboard *keyboard)
{
    gs_usb_keyboard_report item;
    int dropped;
    if (!gs_accepting()) {
        return false;
    }
    memset(&item, 0, sizeof(item));
    item.report_id = GS_REPORT_KEYBOARD;
    item.length = (uint8_t)gs_report_keyboard(keyboard, item.data);
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    dropped = gs_hid_state_queue_push(&gs_keyboard_queue, &item);
    xSemaphoreGive(gs_state_lock);
    xTaskNotifyGive(gs_sender);
    if (dropped) {
        gs_note_drops();
    }
    return true;
}

bool gs_hid_consumer(uint16_t usage)
{
    gs_usb_keyboard_report item;
    int dropped;
    if (!gs_accepting()) {
        return false;
    }
    memset(&item, 0, sizeof(item));
    item.report_id = GS_REPORT_CONSUMER;
    item.length = (uint8_t)gs_report_consumer(usage, item.data);
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    dropped = gs_hid_state_queue_push(&gs_keyboard_queue, &item);
    xSemaphoreGive(gs_state_lock);
    xTaskNotifyGive(gs_sender);
    if (dropped) {
        gs_note_drops();
    }
    return true;
}

bool gs_hid_mouse(const gs_esp32_mouse *mouse)
{
    int dropped;
    if (!gs_accepting()) {
        return false;
    }
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    dropped = gs_hid_mouse_merge_add(&gs_mouse, mouse);
    xSemaphoreGive(gs_state_lock);
    xTaskNotifyGive(gs_sender);
    if (dropped) {
        gs_note_drops();
    }
    return true;
}

bool gs_hid_gamepad(const gs_esp32_gamepad *gamepad)
{
    int dropped;
    if (!gs_accepting()) {
        return false;
    }
    xSemaphoreTake(gs_state_lock, portMAX_DELAY);
    dropped = gs_hid_state_queue_push(&gs_gamepad_queue, gamepad);
    xSemaphoreGive(gs_state_lock);
    xTaskNotifyGive(gs_sender);
    if (dropped) {
        gs_note_drops();
    }
    return true;
}

void gs_hid_phone(gs_esp32_status *status)
{
    if (tud_suspended()) {
        status->usb = GS_ESP32_USB_SUSPENDED;
    } else if (tud_mounted()) {
        status->usb = GS_ESP32_USB_READY;
    } else {
        status->usb = GS_ESP32_USB_DETACHED;
    }
    status->ble = GS_ESP32_BLE_NONE;
    status->peer[0] = '\0';
}

uint32_t gs_hid_features(void)
{
#if CONFIG_IDF_TARGET_ESP32P4
    return GS_ESP32_FEATURE_USB_HID | GS_ESP32_FEATURE_USB_HIGH_SPEED;
#else
    return GS_ESP32_FEATURE_USB_HID;
#endif
}

void gs_hid_rename(const char *name)
{
    (void)name;
}
