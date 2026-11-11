#include "gs/esp32/BlePort.h"

#include <gs/support/Log.h>
#include <gs/support/Thread.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace gs::esp32 {

namespace {

constexpr std::string_view kCategory = "esp32";
constexpr std::size_t kMostPending = 64 * 1024;
constexpr std::size_t kMostFrames = 256;
constexpr std::size_t kSmallestChunk = 20;

}

struct BlePort::Inbox {
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    gs_esp32_record_parser parser{};
    std::deque<std::vector<std::uint8_t>> frames;
    std::vector<BoardKey> keys;
    std::optional<std::size_t> matched;
    std::uint32_t dropped = 0;
    bool gone = false;
    bool interrupted = false;

    void received(std::span<const std::uint8_t> bytes);
    void lose();
    [[nodiscard]] std::optional<std::size_t> keyFor(const std::uint8_t* frame, std::size_t size,
                                                    const std::uint8_t* tag) const;
};

std::optional<std::size_t> BlePort::Inbox::keyFor(const std::uint8_t* frame, std::size_t size,
                                                  const std::uint8_t* tag) const
{
    if (matched) {
        return gs_esp32_ble_tag_matches(keys[*matched].data(), frame, size, tag) ? matched : std::nullopt;
    }
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (gs_esp32_ble_tag_matches(keys[index].data(), frame, size, tag)) {
            return index;
        }
    }
    return std::nullopt;
}

void BlePort::Inbox::received(std::span<const std::uint8_t> bytes)
{
    {
        const std::scoped_lock lock(mutex);
        std::size_t at = 0;
        while (at < bytes.size()) {
            at += gs_esp32_record_parser_feed(&parser, bytes.data() + at, bytes.size() - at);
            gs_esp32_frame frame{};
            const std::uint8_t* tag = nullptr;
            while (gs_esp32_record_parser_next(&parser, &frame, &tag)) {
                const std::uint8_t* start = gs_esp32_frame_bytes(&frame);
                const std::size_t size = gs_esp32_frame_size(&frame);
                const std::optional<std::size_t> key = keyFor(start, size, tag);
                if (!key) {
                    ++dropped;
                    continue;
                }
                matched = key;
                if (frames.size() < kMostFrames) {
                    frames.emplace_back(start, start + size);
                }
            }
        }
    }
    changed.notify_all();
}

void BlePort::Inbox::lose()
{
    {
        const std::scoped_lock lock(mutex);
        gone = true;
    }
    changed.notify_all();
}

struct BlePort::Impl {
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    std::unique_ptr<BleChannel> channel;
    std::mutex sendMutex;
    std::condition_variable_any sendChanged;
    std::vector<std::uint8_t> pending;
    bool failed = false;
    std::jthread writer;
};

BlePort::BlePort(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

BlePort::~BlePort()
{
    if (m_impl->writer.joinable()) {
        m_impl->writer.request_stop();
        m_impl->sendChanged.notify_all();
        m_impl->writer.join();
    }
    m_impl->channel.reset();
}

std::unique_ptr<BlePort> BlePort::open(const BleConnect& connect, std::vector<BoardKey> keys, BleFailure& failure)
{
    failure = BleFailure::None;
    if (!connect || keys.empty()) {
        failure = BleFailure::NotPaired;
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    gs_esp32_record_parser_reset(&impl->inbox->parser);
    impl->inbox->keys = std::move(keys);
    const std::weak_ptr<Inbox> weak = impl->inbox;
    BleChannelEvents events;
    events.received = [weak](std::span<const std::uint8_t> bytes) {
        if (const std::shared_ptr<Inbox> inbox = weak.lock()) {
            inbox->received(bytes);
        }
    };
    events.lost = [weak] {
        if (const std::shared_ptr<Inbox> inbox = weak.lock()) {
            inbox->lose();
        }
    };
    impl->channel = connect(std::move(events), failure);
    if (impl->channel == nullptr) {
        if (failure == BleFailure::None) {
            failure = BleFailure::Unreachable;
        }
        return nullptr;
    }
    failure = BleFailure::None;
    std::unique_ptr<BlePort> port(new BlePort(std::move(impl)));
    Impl& state = *port->m_impl;
    state.writer = std::jthread([&state](const std::stop_token& stop) { pump(state, stop); });
    return port;
}

void BlePort::pump(Impl& impl, const std::stop_token& stop)
{
    platform::setCurrentThreadName("ESP32 Bluetooth");
    std::vector<std::uint8_t> sending;
    for (;;) {
        {
            std::unique_lock lock(impl.sendMutex);
            if (!impl.sendChanged.wait(lock, stop, [&impl] { return !impl.pending.empty(); })) {
                return;
            }
            sending.swap(impl.pending);
        }
        const std::size_t chunk = std::max(impl.channel->chunk(), kSmallestChunk);
        for (std::size_t at = 0; at < sending.size(); at += chunk) {
            const std::size_t size = std::min(chunk, sending.size() - at);
            if (!impl.channel->send(std::span<const std::uint8_t>(sending).subspan(at, size))) {
                {
                    const std::scoped_lock lock(impl.sendMutex);
                    impl.failed = true;
                    impl.pending.clear();
                }
                impl.inbox->lose();
                log::warning(kCategory, "a write to the board over Bluetooth did not go through");
                return;
            }
        }
        sending.clear();
    }
}

bool BlePort::write(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(GS_ESP32_MAX_FRAME)) {
        return false;
    }
    std::vector<BoardKey> keys;
    {
        const std::scoped_lock lock(m_impl->inbox->mutex);
        if (m_impl->inbox->gone) {
            return false;
        }
        if (m_impl->inbox->matched) {
            keys.push_back(m_impl->inbox->keys[*m_impl->inbox->matched]);
        } else {
            keys = m_impl->inbox->keys;
        }
    }
    std::array<std::uint8_t, GS_ESP32_MAX_RECORD> record{};
    {
        const std::scoped_lock lock(m_impl->sendMutex);
        if (m_impl->failed || m_impl->pending.size() > kMostPending) {
            return false;
        }
        for (const BoardKey& key : keys) {
            std::ranges::copy(bytes, record.begin());
            const std::size_t size = gs_esp32_record_seal(record.data(), bytes.size(), record.size(), key.data());
            m_impl->pending.insert(m_impl->pending.end(), record.begin(),
                                   record.begin() + static_cast<std::ptrdiff_t>(size));
        }
    }
    m_impl->sendChanged.notify_all();
    return true;
}

std::size_t BlePort::read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout)
{
    Inbox& inbox = *m_impl->inbox;
    std::unique_lock lock(inbox.mutex);
    static_cast<void>(inbox.changed.wait_for(
        lock, timeout, [&inbox] { return !inbox.frames.empty() || inbox.gone || inbox.interrupted; }));
    if (inbox.frames.empty() || inbox.interrupted) {
        return 0;
    }
    const std::vector<std::uint8_t> frame = std::move(inbox.frames.front());
    inbox.frames.pop_front();
    if (frame.size() > buffer.size()) {
        return 0;
    }
    std::ranges::copy(frame, buffer.begin());
    return frame.size();
}

bool BlePort::setBaud(std::uint32_t)
{
    return false;
}

bool BlePort::setDtr(bool)
{
    return false;
}

bool BlePort::setRts(bool)
{
    return false;
}

void BlePort::discardInput()
{
    const std::scoped_lock lock(m_impl->inbox->mutex);
    m_impl->inbox->frames.clear();
}

void BlePort::interrupt()
{
    {
        const std::scoped_lock lock(m_impl->inbox->mutex);
        m_impl->inbox->interrupted = true;
    }
    m_impl->inbox->changed.notify_all();
}

bool BlePort::datagrams() const
{
    return true;
}

bool BlePort::gone() const
{
    const std::scoped_lock lock(m_impl->inbox->mutex);
    return m_impl->inbox->gone;
}

std::optional<BoardKey> BlePort::matchedKey() const
{
    const std::scoped_lock lock(m_impl->inbox->mutex);
    if (!m_impl->inbox->matched) {
        return std::nullopt;
    }
    return m_impl->inbox->keys[*m_impl->inbox->matched];
}

std::uint32_t BlePort::droppedRecords() const
{
    const std::scoped_lock lock(m_impl->inbox->mutex);
    return m_impl->inbox->dropped;
}

}
