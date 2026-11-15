#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gs_esp32_protocol.h"

#define GS_HID_INTERFACE_TOUCH 0
#define GS_HID_INTERFACE_KEYBOARD 1
#define GS_HID_INTERFACE_MOUSE 2
#define GS_HID_INTERFACE_GAMEPAD 3
#define GS_HID_INTERFACE_COUNT 4
#define GS_HID_INTERFACE_NONE GS_HID_INTERFACE_COUNT

#define GS_HID_TOUCH_STATES 32
#define GS_HID_KEYBOARD_STATES 8
#define GS_HID_GAMEPAD_STATES 8
#define GS_HID_MOUSE_SEGMENTS 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *items;
    uint8_t capacity;
    uint8_t item_size;
    uint8_t head;
    uint8_t count;
    uint32_t dropped;
} gs_hid_state_queue;

void gs_hid_state_queue_init(gs_hid_state_queue *queue, void *items, uint8_t capacity, uint8_t item_size);
int gs_hid_state_queue_push(gs_hid_state_queue *queue, const void *item);
int gs_hid_state_queue_pending(const gs_hid_state_queue *queue);
const void *gs_hid_state_queue_head(const gs_hid_state_queue *queue);
void gs_hid_state_queue_pop(gs_hid_state_queue *queue);
void gs_hid_state_queue_clear(gs_hid_state_queue *queue);
uint32_t gs_hid_state_queue_dropped(const gs_hid_state_queue *queue);

typedef struct {
    uint8_t buttons;
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    int reported;
} gs_hid_mouse_segment;

typedef struct {
    gs_hid_mouse_segment segments[GS_HID_MOUSE_SEGMENTS];
    uint8_t count;
    uint32_t dropped;
} gs_hid_mouse_merge;

void gs_hid_mouse_merge_init(gs_hid_mouse_merge *merge);
int gs_hid_mouse_merge_add(gs_hid_mouse_merge *merge, const gs_esp32_mouse *mouse);
int gs_hid_mouse_merge_pending(const gs_hid_mouse_merge *merge);
int gs_hid_mouse_merge_peek(const gs_hid_mouse_merge *merge, int32_t axis_limit, int8_t wheel_limit,
                            gs_esp32_mouse *report);
void gs_hid_mouse_merge_commit(gs_hid_mouse_merge *merge, const gs_esp32_mouse *report);
void gs_hid_mouse_merge_clear(gs_hid_mouse_merge *merge);
uint32_t gs_hid_mouse_merge_dropped(const gs_hid_mouse_merge *merge);

uint8_t gs_hid_pick_interface(uint8_t pending, uint8_t ready, uint8_t last);

#ifdef __cplusplus
}
#endif
