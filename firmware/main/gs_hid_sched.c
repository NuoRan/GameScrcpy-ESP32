#include "gs_hid_sched.h"

#include <string.h>

void gs_hid_state_queue_init(gs_hid_state_queue *queue, void *items, uint8_t capacity, uint8_t item_size)
{
    queue->items = items;
    queue->capacity = capacity;
    queue->item_size = item_size;
    queue->head = 0;
    queue->count = 0;
    queue->dropped = 0;
}

int gs_hid_state_queue_push(gs_hid_state_queue *queue, const void *item)
{
    uint8_t slot;
    int dropped = 0;
    if (queue->items == NULL || queue->capacity == 0 || queue->item_size == 0) {
        return 0;
    }
    if (queue->count == queue->capacity) {
        queue->head = (uint8_t)((queue->head + 1u) % queue->capacity);
        queue->count--;
        queue->dropped++;
        dropped = 1;
    }
    slot = (uint8_t)((queue->head + queue->count) % queue->capacity);
    memcpy((uint8_t *)queue->items + (size_t)slot * queue->item_size, item, queue->item_size);
    queue->count++;
    return dropped;
}

int gs_hid_state_queue_pending(const gs_hid_state_queue *queue)
{
    return queue->count > 0 ? 1 : 0;
}

const void *gs_hid_state_queue_head(const gs_hid_state_queue *queue)
{
    if (queue->items == NULL || queue->count == 0) {
        return NULL;
    }
    return (const uint8_t *)queue->items + (size_t)queue->head * queue->item_size;
}

void gs_hid_state_queue_pop(gs_hid_state_queue *queue)
{
    if (queue->items == NULL || queue->capacity == 0 || queue->count == 0) {
        return;
    }
    queue->head = (uint8_t)((queue->head + 1u) % queue->capacity);
    queue->count--;
}

void gs_hid_state_queue_clear(gs_hid_state_queue *queue)
{
    queue->head = 0;
    queue->count = 0;
}

uint32_t gs_hid_state_queue_dropped(const gs_hid_state_queue *queue)
{
    return queue->dropped;
}

static int32_t gs_axis_sum(int32_t total, int32_t value)
{
    const int64_t sum = (int64_t)total + (int64_t)value;
    if (sum > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (sum < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)sum;
}

static int16_t gs_clip_axis(int32_t value, int32_t limit)
{
    if (value > limit) {
        return (int16_t)limit;
    }
    if (value < -limit) {
        return (int16_t)(-limit);
    }
    return (int16_t)value;
}

static int8_t gs_clip_wheel(int32_t value, int8_t limit)
{
    if (value > (int32_t)limit) {
        return limit;
    }
    if (value < -(int32_t)limit) {
        return (int8_t)(-(int32_t)limit);
    }
    return (int8_t)value;
}

static void gs_mouse_merge_pop(gs_hid_mouse_merge *merge)
{
    if (merge->count <= 1) {
        merge->count = 0;
        return;
    }
    memmove(&merge->segments[0], &merge->segments[1],
            (size_t)(merge->count - 1) * sizeof(gs_hid_mouse_segment));
    merge->count--;
}

static void gs_mouse_merge_fold(gs_hid_mouse_merge *merge)
{
    gs_hid_mouse_segment *second = &merge->segments[1];
    second->dx = gs_axis_sum(second->dx, merge->segments[0].dx);
    second->dy = gs_axis_sum(second->dy, merge->segments[0].dy);
    second->wheel = gs_axis_sum(second->wheel, merge->segments[0].wheel);
    gs_mouse_merge_pop(merge);
    merge->dropped++;
}

void gs_hid_mouse_merge_init(gs_hid_mouse_merge *merge)
{
    memset(merge, 0, sizeof(*merge));
}

int gs_hid_mouse_merge_add(gs_hid_mouse_merge *merge, const gs_esp32_mouse *mouse)
{
    const uint8_t buttons = (uint8_t)((uint8_t)mouse->buttons & 0x1Fu);
    gs_hid_mouse_segment *last;
    int dropped = 0;
    if (merge->count == 0 || merge->segments[merge->count - 1].buttons != buttons) {
        if (merge->count == GS_HID_MOUSE_SEGMENTS) {
            gs_mouse_merge_fold(merge);
            dropped = 1;
        }
        last = &merge->segments[merge->count];
        last->buttons = buttons;
        last->dx = 0;
        last->dy = 0;
        last->wheel = 0;
        last->reported = 0;
        merge->count++;
    }
    last = &merge->segments[merge->count - 1];
    last->dx = gs_axis_sum(last->dx, mouse->dx);
    last->dy = gs_axis_sum(last->dy, mouse->dy);
    last->wheel = gs_axis_sum(last->wheel, mouse->wheel);
    return dropped;
}

int gs_hid_mouse_merge_pending(const gs_hid_mouse_merge *merge)
{
    return merge->count > 0 ? 1 : 0;
}

int gs_hid_mouse_merge_peek(const gs_hid_mouse_merge *merge, int32_t axis_limit, int8_t wheel_limit,
                            gs_esp32_mouse *report)
{
    const gs_hid_mouse_segment *segment;
    if (merge->count == 0) {
        return 0;
    }
    segment = &merge->segments[0];
    memset(report, 0, sizeof(*report));
    report->buttons = segment->buttons;
    report->dx = gs_clip_axis(segment->dx, axis_limit);
    report->dy = gs_clip_axis(segment->dy, axis_limit);
    report->wheel = gs_clip_wheel(segment->wheel, wheel_limit);
    return 1;
}

void gs_hid_mouse_merge_commit(gs_hid_mouse_merge *merge, const gs_esp32_mouse *report)
{
    gs_hid_mouse_segment *segment;
    if (merge->count == 0) {
        return;
    }
    segment = &merge->segments[0];
    segment->dx -= report->dx;
    segment->dy -= report->dy;
    segment->wheel -= report->wheel;
    segment->reported = 1;
    if (segment->dx == 0 && segment->dy == 0 && segment->wheel == 0) {
        gs_mouse_merge_pop(merge);
    }
}

void gs_hid_mouse_merge_clear(gs_hid_mouse_merge *merge)
{
    merge->count = 0;
}

uint32_t gs_hid_mouse_merge_dropped(const gs_hid_mouse_merge *merge)
{
    return merge->dropped;
}

uint8_t gs_hid_pick_interface(uint8_t pending, uint8_t ready, uint8_t last)
{
    const uint8_t start = (uint8_t)(last % GS_HID_INTERFACE_COUNT);
    uint8_t step;
    for (step = 1; step <= GS_HID_INTERFACE_COUNT; ++step) {
        const uint8_t candidate = (uint8_t)((start + step) % GS_HID_INTERFACE_COUNT);
        const uint8_t bit = (uint8_t)(1u << candidate);
        if ((pending & bit) != 0 && (ready & bit) != 0) {
            return candidate;
        }
    }
    return GS_HID_INTERFACE_NONE;
}
