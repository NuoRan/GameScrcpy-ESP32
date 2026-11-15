#include <gs_hid_descriptors.h>
#include <gs_hid_sched.h>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

gs_esp32_mouse mouseOf(int buttons, int dx, int dy, int wheel)
{
    gs_esp32_mouse mouse{};
    mouse.buttons = static_cast<std::uint8_t>(buttons);
    mouse.dx = static_cast<std::int16_t>(dx);
    mouse.dy = static_cast<std::int16_t>(dy);
    mouse.wheel = static_cast<std::int8_t>(wheel);
    return mouse;
}

gs_esp32_touch touchOf(std::uint8_t id, bool down, std::uint16_t x, std::uint16_t y)
{
    gs_esp32_touch touch{};
    touch.count = 1;
    touch.contacts[0].id = id;
    touch.contacts[0].tip = down ? 1 : 0;
    touch.contacts[0].x = x;
    touch.contacts[0].y = y;
    return touch;
}

std::vector<gs_esp32_mouse> drainMouse(gs_hid_mouse_merge& merge)
{
    std::vector<gs_esp32_mouse> reports;
    gs_esp32_mouse chunk{};
    const auto wheelLimit = static_cast<std::int8_t>(GS_HID_WHEEL_MAX);
    while (reports.size() < 64 &&
           gs_hid_mouse_merge_peek(&merge, static_cast<std::int32_t>(GS_HID_AXIS_MAX), wheelLimit, &chunk)) {
        reports.push_back(chunk);
        gs_hid_mouse_merge_commit(&merge, &chunk);
    }
    return reports;
}

template <typename T, std::size_t N>
void openQueue(gs_hid_state_queue& queue, std::array<T, N>& items)
{
    gs_hid_state_queue_init(&queue, items.data(), static_cast<std::uint8_t>(N),
                            static_cast<std::uint8_t>(sizeof(T)));
}

}

TEST_CASE("R9: mouse motion is merged into one pending report")
{
    gs_hid_mouse_merge merge;
    gs_hid_mouse_merge_init(&merge);
    CHECK_FALSE(gs_hid_mouse_merge_pending(&merge));
    const gs_esp32_mouse first = mouseOf(0, 3, 4, 1);
    const gs_esp32_mouse second = mouseOf(0, 5, -2, 2);
    CHECK_FALSE(gs_hid_mouse_merge_add(&merge, &first));
    CHECK_FALSE(gs_hid_mouse_merge_add(&merge, &second));
    CHECK(gs_hid_mouse_merge_pending(&merge));
    const std::vector<gs_esp32_mouse> reports = drainMouse(merge);
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].buttons == 0);
    CHECK(reports[0].dx == 8);
    CHECK(reports[0].dy == 2);
    CHECK(reports[0].wheel == 3);
    CHECK_FALSE(gs_hid_mouse_merge_pending(&merge));
}

TEST_CASE("R9: a button change goes out after the motion before it")
{
    gs_hid_mouse_merge merge;
    gs_hid_mouse_merge_init(&merge);
    const gs_esp32_mouse move = mouseOf(0, 7, -3, 0);
    const gs_esp32_mouse press = mouseOf(1, 0, 0, 0);
    const gs_esp32_mouse release = mouseOf(0, 0, 0, 0);
    gs_hid_mouse_merge_add(&merge, &move);
    gs_hid_mouse_merge_add(&merge, &press);
    gs_hid_mouse_merge_add(&merge, &release);
    const std::vector<gs_esp32_mouse> reports = drainMouse(merge);
    REQUIRE(reports.size() == 3);
    CHECK(reports[0].buttons == 0);
    CHECK(reports[0].dx == 7);
    CHECK(reports[0].dy == -3);
    CHECK(reports[1].buttons == 1);
    CHECK(reports[1].dx == 0);
    CHECK(reports[2].buttons == 0);
    CHECK(reports[2].dx == 0);
}

TEST_CASE("R9: a click during motion lands after the motion")
{
    gs_hid_mouse_merge merge;
    gs_hid_mouse_merge_init(&merge);
    const gs_esp32_mouse move = mouseOf(0, 12, 0, 0);
    const gs_esp32_mouse press = mouseOf(1, 0, 0, 0);
    gs_hid_mouse_merge_add(&merge, &move);
    gs_hid_mouse_merge_add(&merge, &press);
    const std::vector<gs_esp32_mouse> reports = drainMouse(merge);
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].dx == 12);
    CHECK(reports[0].buttons == 0);
    CHECK(reports[1].buttons == 1);
    CHECK(reports[1].dx == 0);
}

TEST_CASE("R9: motion larger than one report is split, never clipped")
{
    gs_hid_mouse_merge merge;
    gs_hid_mouse_merge_init(&merge);
    int totalDx = 0;
    int totalDy = 0;
    int totalWheel = 0;
    for (int step = 0; step < 4; ++step) {
        const gs_esp32_mouse mouse = mouseOf(0, 30000, -25000, 100);
        totalDx += 30000;
        totalDy -= 25000;
        totalWheel += 100;
        gs_hid_mouse_merge_add(&merge, &mouse);
    }
    const std::vector<gs_esp32_mouse> reports = drainMouse(merge);
    REQUIRE(reports.size() > 1);
    int sumDx = 0;
    int sumDy = 0;
    int sumWheel = 0;
    for (const gs_esp32_mouse& report : reports) {
        CHECK(report.dx <= GS_HID_AXIS_MAX);
        CHECK(report.dx >= -GS_HID_AXIS_MAX);
        CHECK(report.dy <= GS_HID_AXIS_MAX);
        CHECK(report.dy >= -GS_HID_AXIS_MAX);
        CHECK(report.wheel <= GS_HID_WHEEL_MAX);
        CHECK(report.wheel >= -GS_HID_WHEEL_MAX);
        sumDx += report.dx;
        sumDy += report.dy;
        sumWheel += report.wheel;
    }
    CHECK(sumDx == totalDx);
    CHECK(sumDy == totalDy);
    CHECK(sumWheel == totalWheel);
}

TEST_CASE("R9: the mouse segment list is bounded and keeps the motion")
{
    gs_hid_mouse_merge merge;
    gs_hid_mouse_merge_init(&merge);
    int totalDx = 0;
    for (int step = 0; step < GS_HID_MOUSE_SEGMENTS + 4; ++step) {
        const gs_esp32_mouse mouse = mouseOf(step % 2, 5, 0, 0);
        totalDx += 5;
        gs_hid_mouse_merge_add(&merge, &mouse);
    }
    CHECK(merge.count <= GS_HID_MOUSE_SEGMENTS);
    CHECK(gs_hid_mouse_merge_dropped(&merge) != 0U);
    const gs_esp32_mouse last = mouseOf((GS_HID_MOUSE_SEGMENTS + 3) % 2, 0, 0, 0);
    const std::vector<gs_esp32_mouse> reports = drainMouse(merge);
    REQUIRE(!reports.empty());
    CHECK(reports.back().buttons == last.buttons);
    int sumDx = 0;
    for (const gs_esp32_mouse& report : reports) {
        sumDx += report.dx;
    }
    CHECK(sumDx == totalDx);
}

TEST_CASE("R9: the touch queue keeps the newest state when it is full")
{
    std::array<gs_esp32_touch, GS_HID_TOUCH_STATES> items{};
    gs_hid_state_queue queue;
    openQueue(queue, items);
    for (std::uint16_t index = 0; index < GS_HID_TOUCH_STATES; ++index) {
        const gs_esp32_touch state = touchOf(0, true, index, index);
        CHECK_FALSE(gs_hid_state_queue_push(&queue, &state));
    }
    const gs_esp32_touch newest = touchOf(0, true, 100, 100);
    CHECK(gs_hid_state_queue_push(&queue, &newest) == 1);
    CHECK(static_cast<int>(gs_hid_state_queue_dropped(&queue)) == 1);
    const gs_esp32_touch* head = static_cast<const gs_esp32_touch*>(gs_hid_state_queue_head(&queue));
    REQUIRE(head != nullptr);
    CHECK(head->contacts[0].x == 1);
    gs_esp32_touch last{};
    int seen = 0;
    while (gs_hid_state_queue_pending(&queue)) {
        std::memcpy(&last, gs_hid_state_queue_head(&queue), sizeof(last));
        gs_hid_state_queue_pop(&queue);
        ++seen;
    }
    CHECK(seen == GS_HID_TOUCH_STATES);
    CHECK(last.contacts[0].x == 100);
}

TEST_CASE("R9: a lift is never lost behind a full touch queue")
{
    std::array<gs_esp32_touch, GS_HID_TOUCH_STATES> items{};
    gs_hid_state_queue queue;
    openQueue(queue, items);
    gs_esp32_touch holding{};
    holding.count = 2;
    holding.contacts[0] = {0, 1, 100, 200};
    holding.contacts[1] = {1, 1, 300, 400};
    for (int index = 0; index < GS_HID_TOUCH_STATES; ++index) {
        gs_hid_state_queue_push(&queue, &holding);
    }
    gs_esp32_touch lifted{};
    lifted.count = 2;
    lifted.contacts[0] = {0, 1, 100, 200};
    lifted.contacts[1] = {1, 0, 300, 400};
    gs_hid_state_queue_push(&queue, &lifted);
    gs_esp32_touch last{};
    while (gs_hid_state_queue_pending(&queue)) {
        std::memcpy(&last, gs_hid_state_queue_head(&queue), sizeof(last));
        gs_hid_state_queue_pop(&queue);
    }
    REQUIRE(last.count == 2);
    CHECK(last.contacts[0].tip == 1);
    CHECK(last.contacts[1].tip == 0);
    CHECK(last.contacts[1].id == 1);
}

TEST_CASE("R9: keyboard states keep their order")
{
    std::array<gs_esp32_keyboard, GS_HID_KEYBOARD_STATES> items{};
    gs_hid_state_queue queue;
    openQueue(queue, items);
    for (int index = 0; index < GS_HID_KEYBOARD_STATES; ++index) {
        gs_esp32_keyboard keyboard{};
        keyboard.keys[0] = static_cast<std::uint8_t>(index + 1);
        gs_hid_state_queue_push(&queue, &keyboard);
    }
    std::uint8_t expected = 1;
    while (gs_hid_state_queue_pending(&queue)) {
        const gs_esp32_keyboard* head = static_cast<const gs_esp32_keyboard*>(gs_hid_state_queue_head(&queue));
        REQUIRE(head != nullptr);
        CHECK(head->keys[0] == expected);
        ++expected;
        gs_hid_state_queue_pop(&queue);
    }
    CHECK(expected == GS_HID_KEYBOARD_STATES + 1);
}

TEST_CASE("R9: interfaces are served round robin with touch first")
{
    const std::uint8_t all = static_cast<std::uint8_t>((1U << GS_HID_INTERFACE_COUNT) - 1U);
    CHECK(gs_hid_pick_interface(all, all, GS_HID_INTERFACE_GAMEPAD) == GS_HID_INTERFACE_TOUCH);
    CHECK(gs_hid_pick_interface(all, all, GS_HID_INTERFACE_TOUCH) == GS_HID_INTERFACE_KEYBOARD);
    CHECK(gs_hid_pick_interface(all, all, GS_HID_INTERFACE_KEYBOARD) == GS_HID_INTERFACE_MOUSE);
    CHECK(gs_hid_pick_interface(all, all, GS_HID_INTERFACE_MOUSE) == GS_HID_INTERFACE_GAMEPAD);
    CHECK(gs_hid_pick_interface(all, all, GS_HID_INTERFACE_GAMEPAD) == GS_HID_INTERFACE_TOUCH);
}

TEST_CASE("R9: only interfaces with work and a free endpoint are picked")
{
    const std::uint8_t touch = static_cast<std::uint8_t>(1U << GS_HID_INTERFACE_TOUCH);
    const std::uint8_t mouse = static_cast<std::uint8_t>(1U << GS_HID_INTERFACE_MOUSE);
    CHECK(gs_hid_pick_interface(touch | mouse, mouse, GS_HID_INTERFACE_GAMEPAD) == GS_HID_INTERFACE_MOUSE);
    CHECK(gs_hid_pick_interface(touch, touch, GS_HID_INTERFACE_GAMEPAD) == GS_HID_INTERFACE_TOUCH);
    CHECK(gs_hid_pick_interface(touch, mouse, GS_HID_INTERFACE_GAMEPAD) == GS_HID_INTERFACE_NONE);
    CHECK(gs_hid_pick_interface(0, static_cast<std::uint8_t>(0xFFu), GS_HID_INTERFACE_TOUCH) == GS_HID_INTERFACE_NONE);
}

TEST_CASE("R9: clearing a queue leaves it empty and keeps its drop count")
{
    std::array<gs_esp32_gamepad, GS_HID_GAMEPAD_STATES> items{};
    gs_hid_state_queue queue;
    openQueue(queue, items);
    gs_esp32_gamepad gamepad{};
    for (int index = 0; index < GS_HID_GAMEPAD_STATES + 2; ++index) {
        gs_hid_state_queue_push(&queue, &gamepad);
    }
    CHECK(static_cast<int>(gs_hid_state_queue_dropped(&queue)) == 2);
    gs_hid_state_queue_clear(&queue);
    CHECK_FALSE(gs_hid_state_queue_pending(&queue));
    CHECK(static_cast<int>(gs_hid_state_queue_dropped(&queue)) == 2);
    gs_esp32_gamepad stored{};
    gamepad.lt = 9;
    CHECK_FALSE(gs_hid_state_queue_push(&queue, &gamepad));
    std::memcpy(&stored, gs_hid_state_queue_head(&queue), sizeof(stored));
    CHECK(stored.lt == 9);
}
