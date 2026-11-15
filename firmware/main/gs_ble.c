#include "gs_ble.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "gs_board.h"
#include "gs_key.h"
#include "gs_link.h"

#if CONFIG_BT_NIMBLE_HID_SERVICE
#define GS_BLE_HID 1
#else
#define GS_BLE_HID 0
#endif

#define GS_BLE_MOST_CONNECTIONS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define GS_BLE_APPEARANCE 0x03C0
#define GS_BLE_ADVERT_SPACE 31
#define GS_BLE_INBOX 2048
#define GS_BLE_OUTBOX 4096
#define GS_BLE_GATHER (2 * GS_ESP32_MAX_RECORD)
#define GS_BLE_READ_CHUNK 256
#define GS_BLE_TASK_STACK 4096
#define GS_BLE_RETRIES 20
#define GS_BLE_MOST_COMPUTERS 4
#define GS_BLE_NAMESPACE "gs"
#define GS_BLE_COMPUTERS "pcs"
#define GS_BLE_INTERVAL 6
#define GS_BLE_PHONE_INTERVAL 12
#define GS_BLE_TIMEOUT 400

void ble_store_config_init(void);
#if GS_BLE_HID
void gs_hid_ble_limits(void);
#endif

typedef struct {
    uint16_t handle;
    bool computer;
    bool encrypted;
} gs_ble_peer;

static const char *gs_tag = "gs_ble";
static portMUX_TYPE gs_lock = portMUX_INITIALIZER_UNLOCKED;
static gs_ble_peer gs_peers[GS_BLE_MOST_CONNECTIONS];
static volatile uint16_t gs_computer = BLE_HS_CONN_HANDLE_NONE;
static volatile bool gs_prepared;
static volatile bool gs_started;
static volatile bool gs_serving;
static volatile bool gs_synced;
static volatile bool gs_advertising;
static volatile bool gs_for_phone;
static volatile bool gs_fresh;
static uint8_t gs_address_type;
static char gs_name[GS_ESP32_MAX_NAME + 1];
static char gs_peer_name[GS_ESP32_MAX_NAME + 1];
static uint8_t gs_mac[6];
static uint8_t gs_key[GS_ESP32_KEY_SIZE];
static bool gs_keyed;
static uint16_t gs_rx_handle;
static uint16_t gs_tx_handle;
static StreamBufferHandle_t gs_inbox;
static MessageBufferHandle_t gs_outbox;
static ble_addr_t gs_computers[GS_BLE_MOST_COMPUTERS];
static size_t gs_computer_count;

static const ble_uuid128_t gs_service_uuid = BLE_UUID128_INIT(GS_ESP32_BLE_SERVICE_UUID128);
static const ble_uuid128_t gs_rx_uuid = BLE_UUID128_INIT(GS_ESP32_BLE_RX_UUID128);
static const ble_uuid128_t gs_tx_uuid = BLE_UUID128_INIT(GS_ESP32_BLE_TX_UUID128);

static int gs_access(uint16_t connection, uint16_t attribute, struct ble_gatt_access_ctxt *context, void *argument);
static int gs_gap_event(struct ble_gap_event *event, void *argument);

static const struct ble_gatt_chr_def gs_characteristics[] = {
    {
        .uuid = &gs_rx_uuid.u,
        .access_cb = gs_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
        .val_handle = &gs_rx_handle,
    },
    {
        .uuid = &gs_tx_uuid.u,
        .access_cb = gs_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
        .val_handle = &gs_tx_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def gs_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gs_service_uuid.u,
        .characteristics = gs_characteristics,
    },
    {0},
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

static void gs_set_peer_name(const char *text)
{
    portENTER_CRITICAL(&gs_lock);
    gs_copy(gs_peer_name, sizeof(gs_peer_name), text);
    portEXIT_CRITICAL(&gs_lock);
}

static gs_ble_peer *gs_peer_of(uint16_t handle)
{
    for (size_t i = 0; i < GS_BLE_MOST_CONNECTIONS; ++i) {
        if (gs_peers[i].handle == handle) {
            return &gs_peers[i];
        }
    }
    return NULL;
}

static void gs_add_peer(uint16_t handle)
{
    portENTER_CRITICAL(&gs_lock);
    gs_ble_peer *slot = gs_peer_of(BLE_HS_CONN_HANDLE_NONE);
    if (slot != NULL) {
        slot->handle = handle;
        slot->computer = false;
        slot->encrypted = false;
    }
    portEXIT_CRITICAL(&gs_lock);
}

static bool gs_remove_peer(uint16_t handle)
{
    bool computer = false;
    portENTER_CRITICAL(&gs_lock);
    gs_ble_peer *peer = gs_peer_of(handle);
    if (peer != NULL) {
        peer->handle = BLE_HS_CONN_HANDLE_NONE;
        peer->computer = false;
        peer->encrypted = false;
    }
    if (gs_computer == handle) {
        gs_computer = BLE_HS_CONN_HANDLE_NONE;
        computer = true;
    }
    portEXIT_CRITICAL(&gs_lock);
    return computer;
}

static size_t gs_peer_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&gs_lock);
    for (size_t i = 0; i < GS_BLE_MOST_CONNECTIONS; ++i) {
        if (gs_peers[i].handle != BLE_HS_CONN_HANDLE_NONE) {
            ++count;
        }
    }
    portEXIT_CRITICAL(&gs_lock);
    return count;
}

static bool gs_has_phone(void)
{
    bool found = false;
    portENTER_CRITICAL(&gs_lock);
    for (size_t i = 0; i < GS_BLE_MOST_CONNECTIONS; ++i) {
        if (gs_peers[i].handle != BLE_HS_CONN_HANDLE_NONE && !gs_peers[i].computer) {
            found = true;
        }
    }
    portEXIT_CRITICAL(&gs_lock);
    return found;
}

static void gs_mark_computer(uint16_t handle)
{
    portENTER_CRITICAL(&gs_lock);
    gs_ble_peer *peer = gs_peer_of(handle);
    if (peer != NULL) {
        peer->computer = true;
    }
    portEXIT_CRITICAL(&gs_lock);
}

static bool gs_copy_key(uint8_t out[GS_ESP32_KEY_SIZE])
{
    bool keyed = false;
    portENTER_CRITICAL(&gs_lock);
    keyed = gs_keyed;
    if (keyed) {
        memcpy(out, gs_key, GS_ESP32_KEY_SIZE);
    }
    portEXIT_CRITICAL(&gs_lock);
    return keyed;
}

static void gs_load_computers(void)
{
    nvs_handle_t handle;
    size_t size = sizeof(gs_computers);
    gs_computer_count = 0;
    if (nvs_open(GS_BLE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(handle, GS_BLE_COMPUTERS, gs_computers, &size) == ESP_OK) {
        gs_computer_count = size / sizeof(gs_computers[0]);
    }
    nvs_close(handle);
}

static bool gs_known_computer(const ble_addr_t *address)
{
    for (size_t i = 0; i < gs_computer_count; ++i) {
        if (gs_computers[i].type == address->type &&
            memcmp(gs_computers[i].val, address->val, sizeof(address->val)) == 0) {
            return true;
        }
    }
    return false;
}

static void gs_remember_computer(const ble_addr_t *address)
{
    nvs_handle_t handle;
    if (gs_known_computer(address)) {
        return;
    }
    if (gs_computer_count == GS_BLE_MOST_COMPUTERS) {
        memmove(gs_computers, gs_computers + 1, sizeof(gs_computers[0]) * (GS_BLE_MOST_COMPUTERS - 1));
        --gs_computer_count;
    }
    gs_computers[gs_computer_count++] = *address;
    if (nvs_open(GS_BLE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, GS_BLE_COMPUTERS, gs_computers, sizeof(gs_computers[0]) * gs_computer_count) != ESP_OK ||
        nvs_commit(handle) != ESP_OK) {
        ESP_LOGW(gs_tag, "could not remember this computer");
    }
    nvs_close(handle);
}

static void gs_advertise(void)
{
    static const ble_uuid16_t hid_service = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields response;
    struct ble_gap_adv_params parameters;
    gs_esp32_advert advert;
    uint8_t data[sizeof(gs_service_uuid.value) + GS_ESP32_ADVERT_SIZE];
    size_t room = GS_BLE_ADVERT_SPACE - 3 - 2;
    size_t length = 0;
    bool phone = false;
    if (!gs_synced || gs_advertising || ble_gap_adv_active() || gs_peer_count() >= GS_BLE_MOST_CONNECTIONS) {
        return;
    }
    phone = GS_BLE_HID && !gs_has_phone();
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (phone) {
        fields.appearance = GS_BLE_APPEARANCE;
        fields.appearance_is_present = 1;
        fields.uuids16 = &hid_service;
        fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
        room -= 8;
    }
    length = strlen(gs_name);
    fields.name = (uint8_t *)gs_name;
    fields.name_len = (uint8_t)(length > room ? room : length);
    fields.name_is_complete = length <= room;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(gs_tag, "could not set the advertising data");
        return;
    }
    memset(&advert, 0, sizeof(advert));
    memcpy(advert.id, gs_mac, sizeof(advert.id));
    advert.chip = (uint8_t)CONFIG_IDF_FIRMWARE_CHIP_ID;
    advert.flags = (uint8_t)((gs_keyed ? GS_ESP32_ADVERT_KEYED : 0u) |
                             (gs_computer != BLE_HS_CONN_HANDLE_NONE ? GS_ESP32_ADVERT_BUSY : 0u));
    memcpy(data, gs_service_uuid.value, sizeof(gs_service_uuid.value));
    if (gs_esp32_encode_advert(data + sizeof(gs_service_uuid.value), GS_ESP32_ADVERT_SIZE, &advert) == 0) {
        return;
    }
    memset(&response, 0, sizeof(response));
    response.svc_data_uuid128 = data;
    response.svc_data_uuid128_len = (uint8_t)sizeof(data);
    if (ble_gap_adv_rsp_set_fields(&response) != 0) {
        ESP_LOGW(gs_tag, "could not set the scan response");
        return;
    }
    memset(&parameters, 0, sizeof(parameters));
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    parameters.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    if (ble_gap_adv_start(gs_address_type, NULL, BLE_HS_FOREVER, &parameters, gs_gap_event, NULL) == 0) {
        gs_advertising = true;
        gs_for_phone = phone;
    }
}

static void gs_readvertise(void)
{
    if (!gs_synced) {
        return;
    }
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    gs_advertising = false;
    gs_advertise();
}

#if GS_BLE_HID && CONFIG_BT_NIMBLE_GATT_CLIENT
static int gs_peer_name_read(uint16_t connection, const struct ble_gatt_error *error, struct ble_gatt_attr *attribute,
                             void *argument)
{
    (void)connection;
    (void)argument;
    if (error->status == 0 && attribute != NULL) {
        char name[GS_ESP32_MAX_NAME + 1];
        uint16_t length = 0;
        if (ble_hs_mbuf_to_flat(attribute->om, name, GS_ESP32_MAX_NAME, &length) == 0 || length > 0) {
            name[length > GS_ESP32_MAX_NAME ? GS_ESP32_MAX_NAME : length] = '\0';
            if (name[0] != '\0') {
                gs_set_peer_name(name);
            }
        }
    }
    return 0;
}
#endif

static void gs_name_phone(uint16_t connection, const struct ble_gap_conn_desc *description)
{
    char address[18];
    const uint8_t *value = description->peer_id_addr.val;
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", value[5], value[4], value[3], value[2],
             value[1], value[0]);
    gs_set_peer_name(address);
#if GS_BLE_HID && CONFIG_BT_NIMBLE_GATT_CLIENT
    ble_gattc_read_by_uuid(connection, 1, 0xFFFF, BLE_UUID16_DECLARE(BLE_SVC_GAP_CHR_UUID16_DEVICE_NAME),
                           gs_peer_name_read, NULL);
#else
    (void)connection;
#endif
}

static void gs_request_interval(uint16_t connection, uint16_t longest)
{
    const struct ble_gap_upd_params update = {
        .itvl_min = GS_BLE_INTERVAL,
        .itvl_max = longest,
        .latency = 0,
        .supervision_timeout = GS_BLE_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    ble_gap_update_params(connection, &update);
}

static void gs_report_link(uint16_t connection)
{
    struct ble_gap_conn_desc description;
    unsigned interval = 0;
    if (ble_gap_conn_find(connection, &description) != 0) {
        return;
    }
    interval = (unsigned)description.conn_itvl * 125u;
    ESP_LOGI(gs_tag, "computer link: every %u.%02u ms, MTU %u", interval / 100u, interval % 100u,
             (unsigned)ble_att_mtu(connection));
}

static void gs_become_computer(uint16_t connection)
{
    struct ble_gap_conn_desc description;
    const bool found = ble_gap_conn_find(connection, &description) == 0;
    bool fresh = false;
    portENTER_CRITICAL(&gs_lock);
    gs_ble_peer *peer = gs_peer_of(connection);
    if (peer != NULL) {
        peer->computer = true;
    }
    fresh = gs_computer != connection;
    gs_computer = connection;
    portEXIT_CRITICAL(&gs_lock);
    if (found && description.sec_state.bonded) {
        gs_remember_computer(&description.peer_id_addr);
    }
    if (fresh) {
        gs_fresh = true;
        gs_request_interval(connection, GS_BLE_INTERVAL);
        ESP_LOGI(gs_tag, "a computer is connected over Bluetooth");
        gs_readvertise();
    }
}

static void gs_secured(uint16_t connection)
{
    struct ble_gap_conn_desc description;
    bool known = false;
    bool computer = false;
    if (ble_gap_conn_find(connection, &description) != 0) {
        return;
    }
    known = gs_known_computer(&description.peer_id_addr);
    portENTER_CRITICAL(&gs_lock);
    gs_ble_peer *peer = gs_peer_of(connection);
    if (peer != NULL) {
        peer->encrypted = true;
        peer->computer = peer->computer || known;
        computer = peer->computer;
    }
    portEXIT_CRITICAL(&gs_lock);
    if (!computer && GS_BLE_HID) {
        gs_name_phone(connection, &description);
    }
    gs_readvertise();
}

static int gs_gap_event(struct ble_gap_event *event, void *argument)
{
    struct ble_gap_conn_desc description;
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        gs_advertising = false;
        if (event->connect.status == 0) {
            gs_add_peer(event->connect.conn_handle);
            if (ble_gap_conn_find(event->connect.conn_handle, &description) == 0 &&
                gs_known_computer(&description.peer_id_addr)) {
                gs_mark_computer(event->connect.conn_handle);
            }
#if GS_BLE_HID
            gs_hid_ble_limits();
#endif
            gs_request_interval(event->connect.conn_handle, GS_BLE_PHONE_INTERVAL);
#if CONFIG_BT_NIMBLE_GATT_CLIENT
            ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
#endif
        }
        gs_advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        if (gs_remove_peer(event->disconnect.conn.conn_handle)) {
            ESP_LOGI(gs_tag, "the computer's Bluetooth link ended");
            gs_board_link_lost(GS_LINK_BLE);
        }
        if (!gs_has_phone()) {
            gs_set_peer_name("");
        }
        gs_readvertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        gs_advertising = false;
        gs_advertise();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            gs_secured(event->enc_change.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == gs_tx_handle && event->subscribe.cur_notify) {
            gs_become_computer(event->subscribe.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0 && event->conn_update.conn_handle == gs_computer) {
            gs_report_link(event->conn_update.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == gs_computer) {
            gs_report_link(event->mtu.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &description) == 0) {
            ble_store_util_delete_peer(&description.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static int gs_access(uint16_t connection, uint16_t attribute, struct ble_gatt_access_ctxt *context, void *argument)
{
    static uint8_t chunk[GS_ESP32_MAX_PAYLOAD];
    uint16_t copied = 0;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attribute != gs_rx_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (connection != gs_computer || gs_inbox == NULL) {
        return 0;
    }
    ble_hs_mbuf_to_flat(context->om, chunk, sizeof(chunk), &copied);
    if (copied > 0 && xStreamBufferSend(gs_inbox, chunk, copied, 0) != copied) {
        ESP_LOGW(gs_tag, "the computer sent faster than the board reads; bytes were dropped");
    }
    return 0;
}

static void gs_inbox_task(void *argument)
{
    static gs_esp32_record_parser parser;
    static uint8_t chunk[GS_BLE_READ_CHUNK];
    uint8_t key[GS_ESP32_KEY_SIZE];
    gs_esp32_frame frame;
    const uint8_t *tag = NULL;
    (void)argument;
    gs_esp32_record_parser_reset(&parser);
    for (;;) {
        const size_t received = xStreamBufferReceive(gs_inbox, chunk, sizeof(chunk), portMAX_DELAY);
        size_t at = 0;
        if (gs_fresh) {
            gs_fresh = false;
            gs_esp32_record_parser_reset(&parser);
        }
        while (at < received) {
            at += gs_esp32_record_parser_feed(&parser, chunk + at, received - at);
            while (gs_esp32_record_parser_next(&parser, &frame, &tag)) {
                if (gs_copy_key(key) && gs_esp32_ble_tag_matches(key, gs_esp32_frame_bytes(&frame),
                                                                 gs_esp32_frame_size(&frame), tag)) {
                    gs_link_received(GS_LINK_BLE, &frame);
                }
            }
        }
    }
}

static void gs_notify(uint16_t connection, const uint8_t *data, size_t size)
{
    const uint16_t mtu = ble_att_mtu(connection);
    const size_t most = mtu > 23 ? (size_t)mtu - 3 : 20;
    size_t at = 0;
    while (at < size) {
        const size_t take = size - at < most ? size - at : most;
        int attempt = 0;
        for (;;) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + at, (uint16_t)take);
            if (om != NULL && ble_gatts_notify_custom(connection, gs_tx_handle, om) == 0) {
                break;
            }
            if (++attempt >= GS_BLE_RETRIES || gs_computer != connection) {
                return;
            }
            vTaskDelay(1);
        }
        at += take;
    }
}

static void gs_outbox_task(void *argument)
{
    static uint8_t frame[GS_ESP32_MAX_FRAME];
    static uint8_t gathered[GS_BLE_GATHER];
    uint8_t key[GS_ESP32_KEY_SIZE];
    (void)argument;
    for (;;) {
        size_t used = 0;
        size_t size = xMessageBufferReceive(gs_outbox, frame, sizeof(frame), portMAX_DELAY);
        while (size > 0) {
            if (gs_copy_key(key)) {
                memcpy(gathered + used, frame, size);
                used += gs_esp32_record_seal(gathered + used, size, sizeof(gathered) - used, key);
            }
            size = sizeof(gathered) - used >= GS_ESP32_MAX_RECORD
                       ? xMessageBufferReceive(gs_outbox, frame, sizeof(frame), 0)
                       : 0;
        }
        const uint16_t connection = gs_computer;
        if (used > 0 && connection != BLE_HS_CONN_HANDLE_NONE) {
            gs_notify(connection, gathered, used);
        }
    }
}

static void gs_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void gs_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &gs_address_type);
    gs_synced = true;
    gs_advertise();
}

static void gs_on_reset(int reason)
{
    gs_synced = false;
    gs_advertising = false;
    ESP_LOGW(gs_tag, "Bluetooth host reset (%d)", reason);
}

bool gs_ble_prepare(const char *name, const uint8_t mac[6])
{
    esp_err_t result;
    if (gs_prepared) {
        return true;
    }
    gs_copy(gs_name, sizeof(gs_name), name);
    memcpy(gs_mac, mac, sizeof(gs_mac));
    for (size_t i = 0; i < GS_BLE_MOST_CONNECTIONS; ++i) {
        gs_peers[i].handle = BLE_HS_CONN_HANDLE_NONE;
    }
    gs_keyed = gs_key_load(gs_key);
    gs_load_computers();
    result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(gs_tag, "Bluetooth could not start (%s); the board keeps running without it", esp_err_to_name(result));
        return false;
    }
    ble_hs_cfg.reset_cb = gs_on_reset;
    ble_hs_cfg.sync_cb = gs_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_att_set_preferred_mtu(GS_ESP32_BLE_MTU);
    gs_prepared = true;
    return true;
}

bool gs_ble_start(void)
{
    int added = -1;
    if (!gs_prepared || gs_started) {
        return gs_started;
    }
#if !GS_BLE_HID
    ble_svc_gap_init();
    ble_svc_gatt_init();
#endif
    gs_inbox = xStreamBufferCreate(GS_BLE_INBOX, 1);
    gs_outbox = xMessageBufferCreate(GS_BLE_OUTBOX);
    if (gs_inbox != NULL && gs_outbox != NULL) {
        added = ble_gatts_count_cfg(gs_services);
        if (added == 0) {
            added = ble_gatts_add_svcs(gs_services);
        }
    }
    if (added == 0 &&
        xTaskCreate(gs_inbox_task, "gs_ble_in", GS_BLE_TASK_STACK, NULL, configMAX_PRIORITIES - 3, NULL) == pdPASS &&
        xTaskCreate(gs_outbox_task, "gs_ble_out", GS_BLE_TASK_STACK, NULL, configMAX_PRIORITIES - 3, NULL) == pdPASS) {
        gs_serving = true;
    } else {
        ESP_LOGE(gs_tag, "the Bluetooth link to the computer could not start; the board keeps running without it");
    }
    ble_svc_gap_device_name_set(gs_name);
    ble_store_config_init();
    nimble_port_freertos_init(gs_host_task);
    gs_started = true;
    return true;
}

bool gs_ble_link_start(const char *name, const uint8_t mac[6])
{
#if GS_BLE_HID
    (void)name;
    (void)mac;
#else
    if (!gs_started && gs_ble_prepare(name, mac)) {
        (void)gs_ble_start();
    }
#endif
    return gs_ble_link_available();
}

bool gs_ble_link_available(void)
{
    return gs_started && gs_serving;
}

void gs_ble_link_send(const uint8_t *frame, size_t size)
{
    if (!gs_serving || gs_outbox == NULL || gs_computer == BLE_HS_CONN_HANDLE_NONE || size > GS_ESP32_MAX_FRAME) {
        return;
    }
    (void)xMessageBufferSend(gs_outbox, frame, size, 0);
}

void gs_ble_set_key(const uint8_t key[GS_ESP32_KEY_SIZE])
{
    portENTER_CRITICAL(&gs_lock);
    memcpy(gs_key, key, GS_ESP32_KEY_SIZE);
    gs_keyed = true;
    portEXIT_CRITICAL(&gs_lock);
    gs_readvertise();
}

void gs_ble_forget_key(void)
{
    portENTER_CRITICAL(&gs_lock);
    memset(gs_key, 0, GS_ESP32_KEY_SIZE);
    gs_keyed = false;
    portEXIT_CRITICAL(&gs_lock);
    gs_readvertise();
}

void gs_ble_rename(const char *name)
{
    gs_copy(gs_name, sizeof(gs_name), name);
    if (!gs_started) {
        return;
    }
    ble_svc_gap_device_name_set(gs_name);
    gs_readvertise();
}

bool gs_ble_signal(int8_t *signal)
{
    const uint16_t connection = gs_computer;
    return connection != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_rssi(connection, signal) == 0;
}

uint16_t gs_ble_phone(void)
{
    uint16_t found = BLE_HS_CONN_HANDLE_NONE;
    portENTER_CRITICAL(&gs_lock);
    for (size_t i = 0; i < GS_BLE_MOST_CONNECTIONS && found == BLE_HS_CONN_HANDLE_NONE; ++i) {
        if (gs_peers[i].handle != BLE_HS_CONN_HANDLE_NONE && !gs_peers[i].computer && gs_peers[i].encrypted) {
            found = gs_peers[i].handle;
        }
    }
    portEXIT_CRITICAL(&gs_lock);
    return found;
}

void gs_ble_phone_status(gs_esp32_status *status)
{
    const bool phone = gs_has_phone();
    if (phone) {
        status->ble = GS_ESP32_BLE_CONNECTED;
    } else if (gs_advertising && gs_for_phone) {
        status->ble = GS_ESP32_BLE_ADVERTISING;
    } else {
        status->ble = GS_ESP32_BLE_IDLE;
    }
    portENTER_CRITICAL(&gs_lock);
    memcpy(status->peer, gs_peer_name, sizeof(status->peer));
    portEXIT_CRITICAL(&gs_lock);
    if (!phone) {
        status->peer[0] = '\0';
    }
}

#else

bool gs_ble_prepare(const char *name, const uint8_t mac[6])
{
    (void)name;
    (void)mac;
    return false;
}

bool gs_ble_start(void)
{
    return false;
}

bool gs_ble_link_start(const char *name, const uint8_t mac[6])
{
    (void)name;
    (void)mac;
    return false;
}

bool gs_ble_link_available(void)
{
    return false;
}

void gs_ble_link_send(const uint8_t *frame, size_t size)
{
    (void)frame;
    (void)size;
}

void gs_ble_set_key(const uint8_t key[GS_ESP32_KEY_SIZE])
{
    (void)key;
}

void gs_ble_forget_key(void) {}

void gs_ble_rename(const char *name)
{
    (void)name;
}

bool gs_ble_signal(int8_t *signal)
{
    (void)signal;
    return false;
}

uint16_t gs_ble_phone(void)
{
    return GS_BLE_NO_CONNECTION;
}

void gs_ble_phone_status(gs_esp32_status *status)
{
    status->ble = GS_ESP32_BLE_NONE;
    status->peer[0] = '\0';
}

#endif
