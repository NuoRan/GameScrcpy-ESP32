#pragma once

#include "TestSupport.h"

#include <gs/esp32/Bluetooth.h>
#include <gs/esp32/Boards.h>

#include <gs_esp32_protocol.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace esp32test {

class FakeBleBoard
{
public:
    struct Options {
        gs::esp32::BoardKey key{};
        bool answers = true;
        std::size_t chunk = 20;
        int signal = -58;
        std::array<std::uint8_t, 6> mac{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    };

    explicit FakeBleBoard(Options options)
        : m_options(options)
    {
        gs_esp32_record_parser_reset(&m_parser);
    }

    void attach(gs::esp32::BleChannelEvents events)
    {
        const std::lock_guard lock(m_mutex);
        m_events = std::move(events);
    }

    void received(std::span<const std::uint8_t> bytes)
    {
        Bytes replies;
        gs::esp32::BleChannelEvents events;
        {
            const std::lock_guard lock(m_mutex);
            m_writes.push_back(bytes.size());
            std::size_t at = 0;
            while (at < bytes.size()) {
                at += gs_esp32_record_parser_feed(&m_parser, bytes.data() + at, bytes.size() - at);
                gs_esp32_frame frame{};
                const std::uint8_t* tag = nullptr;
                while (gs_esp32_record_parser_next(&m_parser, &frame, &tag)) {
                    if (!gs_esp32_ble_tag_matches(m_options.key.data(), gs_esp32_frame_bytes(&frame),
                                                  gs_esp32_frame_size(&frame), tag)) {
                        ++m_badTags;
                        continue;
                    }
                    ++m_records;
                    m_types.push_back(frame.type);
                    handle(frame, replies);
                }
            }
            events = m_events;
        }
        deliver(events, replies);
    }

    void push(const Bytes& bytes)
    {
        gs::esp32::BleChannelEvents events;
        {
            const std::lock_guard lock(m_mutex);
            events = m_events;
        }
        deliver(events, bytes);
    }

    void drop()
    {
        gs::esp32::BleChannelEvents events;
        {
            const std::lock_guard lock(m_mutex);
            events = m_events;
        }
        if (events.lost) {
            events.lost();
        }
    }

    [[nodiscard]] static Bytes sealed(std::uint8_t type, const Bytes& payload, const gs::esp32::BoardKey& key)
    {
        std::array<std::uint8_t, GS_ESP32_MAX_RECORD> record{};
        const std::size_t size =
            gs_esp32_frame_encode(record.data(), record.size(), type, 0, payload.data(), payload.size());
        const std::size_t total = gs_esp32_record_seal(record.data(), size, record.size(), key.data());
        return Bytes(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(total));
    }

    [[nodiscard]] std::size_t count(std::uint8_t type) const
    {
        const std::lock_guard lock(m_mutex);
        return static_cast<std::size_t>(std::ranges::count(m_types, type));
    }

    [[nodiscard]] std::vector<std::size_t> writes() const
    {
        const std::lock_guard lock(m_mutex);
        return m_writes;
    }

    [[nodiscard]] int badTags() const
    {
        const std::lock_guard lock(m_mutex);
        return m_badTags;
    }

    [[nodiscard]] int records() const
    {
        const std::lock_guard lock(m_mutex);
        return m_records;
    }

    [[nodiscard]] std::size_t chunk() const noexcept { return m_options.chunk; }

    [[nodiscard]] gs::esp32::SeenBoard seen(std::uint64_t address) const
    {
        gs::esp32::SeenBoard board;
        board.address = address;
        board.name = "Fake C3";
        board.advert = gs::esp32::BoardAdvert{.id = gs::esp32::boardId(m_options.mac),
                                              .chipId = GS_ESP32_CHIP_ESP32C3,
                                              .keyed = true,
                                              .busy = false};
        board.signal = m_options.signal;
        board.at = std::chrono::steady_clock::now();
        return board;
    }

private:
    void handle(const gs_esp32_frame& frame, Bytes& replies)
    {
        if (!m_options.answers) {
            return;
        }
        std::array<std::uint8_t, GS_ESP32_MAX_PAYLOAD> payload{};
        std::size_t size = 0;
        std::uint8_t type = 0;
        if (frame.type == GS_ESP32_MSG_HELLO_REQ) {
            gs_esp32_hello hello{};
            hello.protocol = GS_ESP32_PROTOCOL_VERSION;
            std::memcpy(hello.firmware, "1.1.0", 5);
            hello.chip_id = GS_ESP32_CHIP_ESP32C3;
            hello.chip_revision = 4;
            hello.features = GS_ESP32_FEATURE_BLE_HID | GS_ESP32_FEATURE_WIFI | GS_ESP32_FEATURE_BLE_LINK |
                             GS_ESP32_FEATURE_KEYED;
            std::copy(m_options.mac.begin(), m_options.mac.end(), hello.mac);
            hello.wifi = GS_ESP32_WIFI_OFF;
            std::memcpy(hello.name, "Fake C3", 7);
            size = gs_esp32_encode_hello(payload.data(), payload.size(), &hello);
            type = GS_ESP32_MSG_HELLO;
        } else if (frame.type == GS_ESP32_MSG_PING) {
            std::uint32_t token = 0;
            if (!gs_esp32_decode_token(frame.payload, frame.length, &token)) {
                return;
            }
            size = gs_esp32_encode_pong(payload.data(), payload.size(), token,
                                        static_cast<std::int8_t>(m_options.signal));
            type = GS_ESP32_MSG_PONG;
        } else {
            return;
        }
        std::array<std::uint8_t, GS_ESP32_MAX_RECORD> record{};
        const std::size_t length =
            gs_esp32_frame_encode(record.data(), record.size(), type, m_seq++, payload.data(), size);
        const std::size_t total = gs_esp32_record_seal(record.data(), length, record.size(), m_options.key.data());
        replies.insert(replies.end(), record.begin(), record.begin() + static_cast<std::ptrdiff_t>(total));
    }

    void deliver(const gs::esp32::BleChannelEvents& events, const Bytes& bytes) const
    {
        if (!events.received) {
            return;
        }
        for (std::size_t at = 0; at < bytes.size(); at += m_options.chunk) {
            const std::size_t size = std::min(m_options.chunk, bytes.size() - at);
            events.received(std::span<const std::uint8_t>(bytes).subspan(at, size));
        }
    }

    Options m_options;
    mutable std::mutex m_mutex;
    gs::esp32::BleChannelEvents m_events;
    gs_esp32_record_parser m_parser{};
    std::vector<std::size_t> m_writes;
    std::vector<std::uint8_t> m_types;
    std::uint8_t m_seq = 0;
    int m_badTags = 0;
    int m_records = 0;
};

class FakeBleChannel final : public gs::esp32::BleChannel
{
public:
    explicit FakeBleChannel(std::shared_ptr<FakeBleBoard> board, std::shared_ptr<std::atomic<bool>> held = nullptr)
        : m_board(std::move(board))
        , m_held(std::move(held))
    {
    }

    [[nodiscard]] std::size_t chunk() const override { return m_board->chunk(); }

    [[nodiscard]] bool send(std::span<const std::uint8_t> bytes) override
    {
        if (m_held != nullptr) {
            m_waiting.store(true);
            while (m_held->load()) {
                std::this_thread::yield();
            }
            m_waiting.store(false);
        }
        m_board->received(bytes);
        return true;
    }

    [[nodiscard]] bool waiting() const noexcept { return m_waiting.load(); }

private:
    std::shared_ptr<FakeBleBoard> m_board;
    std::shared_ptr<std::atomic<bool>> m_held;
    std::atomic<bool> m_waiting{false};
};

class FakeRadio final : public gs::esp32::BleRadio
{
public:
    void add(std::uint64_t address, std::shared_ptr<FakeBleBoard> board, bool advertising = true)
    {
        const std::lock_guard lock(m_mutex);
        m_boards[address] = std::move(board);
        if (advertising) {
            m_advertising.insert(address);
        }
    }

    void setState(gs::esp32::BluetoothState state)
    {
        const std::lock_guard lock(m_mutex);
        m_state = state;
    }

    void setPairing(gs::esp32::PairOutcome outcome)
    {
        const std::lock_guard lock(m_mutex);
        m_pairing = outcome;
    }

    void pairElsewhere(std::uint64_t address)
    {
        const std::lock_guard lock(m_mutex);
        m_paired.insert(address);
    }

    [[nodiscard]] gs::esp32::BluetoothState state() override
    {
        const std::lock_guard lock(m_mutex);
        return m_state;
    }

    void watch(bool on) override
    {
        const std::lock_guard lock(m_mutex);
        m_watchers += on ? 1 : -1;
    }

    [[nodiscard]] std::vector<gs::esp32::SeenBoard> seen(std::chrono::milliseconds) override
    {
        const std::lock_guard lock(m_mutex);
        std::vector<gs::esp32::SeenBoard> found;
        for (const std::uint64_t address : m_advertising) {
            found.push_back(m_boards.at(address)->seen(address));
        }
        return found;
    }

    [[nodiscard]] bool find(std::uint64_t address, std::chrono::milliseconds, const std::stop_token&) override
    {
        const std::lock_guard lock(m_mutex);
        return m_advertising.contains(address);
    }

    [[nodiscard]] bool linked(std::uint64_t) override { return false; }

    [[nodiscard]] bool paired(std::uint64_t address) override
    {
        const std::lock_guard lock(m_mutex);
        return m_paired.contains(address);
    }

    [[nodiscard]] gs::esp32::PairOutcome pair(std::uint64_t address) override
    {
        const std::lock_guard lock(m_mutex);
        if (!m_boards.contains(address)) {
            return gs::esp32::PairOutcome::NotFound;
        }
        if (m_pairing == gs::esp32::PairOutcome::Paired) {
            m_paired.insert(address);
        }
        return m_pairing;
    }

    [[nodiscard]] bool unpair(std::uint64_t address) override
    {
        const std::lock_guard lock(m_mutex);
        return m_paired.erase(address) > 0;
    }

    [[nodiscard]] std::unique_ptr<gs::esp32::BleChannel> open(std::uint64_t address,
                                                              gs::esp32::BleChannelEvents events,
                                                              gs::esp32::BleFailure& failure) override
    {
        std::shared_ptr<FakeBleBoard> board;
        {
            const std::lock_guard lock(m_mutex);
            const auto found = m_boards.find(address);
            if (found == m_boards.end()) {
                failure = gs::esp32::BleFailure::NotFound;
                return nullptr;
            }
            board = found->second;
            ++m_opened;
        }
        board->attach(std::move(events));
        failure = gs::esp32::BleFailure::None;
        return std::make_unique<FakeBleChannel>(std::move(board));
    }

    [[nodiscard]] int watchers() const
    {
        const std::lock_guard lock(m_mutex);
        return m_watchers;
    }

    [[nodiscard]] int opened() const
    {
        const std::lock_guard lock(m_mutex);
        return m_opened;
    }

    [[nodiscard]] bool isPaired(std::uint64_t address) const
    {
        const std::lock_guard lock(m_mutex);
        return m_paired.contains(address);
    }

private:
    mutable std::mutex m_mutex;
    gs::esp32::BluetoothState m_state = gs::esp32::BluetoothState::Ready;
    gs::esp32::PairOutcome m_pairing = gs::esp32::PairOutcome::Paired;
    std::map<std::uint64_t, std::shared_ptr<FakeBleBoard>> m_boards;
    std::set<std::uint64_t> m_advertising;
    std::set<std::uint64_t> m_paired;
    int m_watchers = 0;
    int m_opened = 0;
};

}
