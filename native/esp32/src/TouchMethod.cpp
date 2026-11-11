#include "gs/esp32/TouchMethod.h"

#include "gs/esp32/BlePort.h"
#include "gs/esp32/Boards.h"
#include "gs/esp32/Discovery.h"
#include "gs/esp32/UdpPort.h"

#include <gs/support/Log.h>

#include <algorithm>
#include <cctype>

namespace gs::esp32 {

namespace {

constexpr std::string_view kCategory = "esp32";
constexpr std::chrono::milliseconds kBrowseTime{2000};
constexpr std::chrono::milliseconds kSettleMargin{3000};
constexpr std::chrono::milliseconds kSettleSlice{50};
constexpr std::chrono::milliseconds kFindTime{3000};
constexpr std::chrono::milliseconds kBluetoothGiveUp{4000};
constexpr int kMostMouseStep = 32767;
constexpr int kMostWheel = 127;

bool automatic(std::string_view port) noexcept
{
    return port.empty() || port == kAutomaticPort;
}

LinkOptions serialOptions(PortKind kind, bool allowLegacy)
{
    LinkOptions options;
    options.fastBaud = bridge(kind);
    options.allowLegacy = allowLegacy;
    return options;
}

OpenFailure failureOf(SerialFailure failure) noexcept
{
    switch (failure) {
    case SerialFailure::Busy:
        return OpenFailure::PortBusy;
    case SerialFailure::NotFound:
        return OpenFailure::PortMissing;
    default:
        return OpenFailure::NoAnswer;
    }
}

LinkState settle(const Esp32Link& link, std::chrono::milliseconds timeout, const std::stop_token& stop)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    LinkState state = link.state();
    while (state == LinkState::Opening && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        state = link.waitSettled(kSettleSlice);
    }
    return state;
}

std::optional<OpenedLink> tryPort(const SerialPortInfo& port, bool allowLegacy, const LinkEvents& events,
                                  SerialFailure& failure, const std::stop_token& stop)
{
    std::unique_ptr<Port> opened = openSerialPort(port.name, GS_ESP32_HANDSHAKE_BAUD, failure);
    if (!opened) {
        return std::nullopt;
    }
    LinkOptions options = serialOptions(port.kind, allowLegacy);
    auto link = std::make_unique<Esp32Link>(std::move(opened), options, events);
    const LinkState settled = settle(*link, options.helloWait + kSettleMargin, stop);
    if (settled != LinkState::Ready && settled != LinkState::Legacy) {
        return std::nullopt;
    }
    return OpenedLink{.link = std::move(link), .where = port.name, .kind = port.kind, .mode = LinkMode::Serial};
}

Expected<OpenedLink, OpenFailure> openSerial(const Esp32Settings& settings, const LinkEvents& events,
                                             const std::stop_token& stop)
{
    const std::vector<SerialPortInfo> ports = listSerialPorts();
    if (!automatic(settings.port)) {
        if (!validPortName(settings.port)) {
            return makeUnexpected(OpenFailure::PortMissing);
        }
        SerialPortInfo chosen{.name = settings.port};
        const auto listed = std::ranges::find_if(ports, [&settings](const SerialPortInfo& port) {
            return portNumber(port.name) == portNumber(settings.port);
        });
        if (listed != ports.end()) {
            chosen = *listed;
        }
        SerialFailure failure = SerialFailure::None;
        if (std::optional<OpenedLink> opened = tryPort(chosen, true, events, failure, stop)) {
            return std::move(*opened);
        }
        if (stop.stop_requested()) {
            return makeUnexpected(OpenFailure::Stopped);
        }
        return makeUnexpected(failure == SerialFailure::None ? OpenFailure::NoAnswer : failureOf(failure));
    }
    std::optional<SerialPortInfo> legacyCandidate;
    bool busy = false;
    for (const SerialPortInfo& port : ports) {
        if (stop.stop_requested()) {
            return makeUnexpected(OpenFailure::Stopped);
        }
        if (!probedAutomatically(port.kind)) {
            continue;
        }
        SerialFailure failure = SerialFailure::None;
        if (std::optional<OpenedLink> opened = tryPort(port, false, events, failure, stop)) {
            return std::move(*opened);
        }
        busy = busy || failure == SerialFailure::Busy;
        if (failure == SerialFailure::None && bridge(port.kind) && !legacyCandidate) {
            legacyCandidate = port;
        }
    }
    if (legacyCandidate && !stop.stop_requested()) {
        SerialFailure failure = SerialFailure::None;
        if (std::optional<OpenedLink> opened = tryPort(*legacyCandidate, true, events, failure, stop)) {
            return std::move(*opened);
        }
    }
    if (stop.stop_requested()) {
        return makeUnexpected(OpenFailure::Stopped);
    }
    return makeUnexpected(busy ? OpenFailure::PortBusy : OpenFailure::NoBoard);
}

Expected<OpenedLink, OpenFailure> openWifi(const Esp32Settings& settings, const LinkEvents& events,
                                           const std::stop_token& stop)
{
    const std::vector<KnownBoard>& known = settings.knownBoards;
    if (known.empty()) {
        return makeUnexpected(OpenFailure::NoKey);
    }
    std::optional<HostAndPort> target;
    std::vector<BoardKey> keys;
    if (settings.address.empty()) {
        for (const FoundBoard& found : browseBoards(kBrowseTime, stop)) {
            const std::optional<KnownBoard> board = findBoard(known, found.id);
            if (board) {
                target = HostAndPort{.host = found.address, .port = found.port};
                keys.push_back(board->key);
                break;
            }
        }
        if (stop.stop_requested()) {
            return makeUnexpected(OpenFailure::Stopped);
        }
        if (!target) {
            return makeUnexpected(OpenFailure::NoBoard);
        }
    } else {
        target = parseAddress(settings.address, GS_ESP32_UDP_PORT);
        if (!target) {
            return makeUnexpected(OpenFailure::BadAddress);
        }
        const auto remembered = std::ranges::find_if(known, [&settings](const KnownBoard& board) {
            return !board.address.empty() && board.address == settings.address;
        });
        if (remembered != known.end()) {
            keys.push_back(remembered->key);
        }
        for (const KnownBoard& board : known) {
            if (std::ranges::find(keys, board.key) == keys.end()) {
                keys.push_back(board.key);
            }
        }
    }
    std::unique_ptr<UdpPort> port = UdpPort::open(*target, keys);
    if (!port) {
        return makeUnexpected(OpenFailure::BadAddress);
    }
    const std::string where = port->peer();
    LinkOptions options;
    options.allowLegacy = false;
    auto link = std::make_unique<Esp32Link>(std::move(port), options, events);
    if (settle(*link, options.udpGiveUp + kSettleMargin, stop) != LinkState::Ready) {
        return makeUnexpected(stop.stop_requested() ? OpenFailure::Stopped : OpenFailure::NoAnswer);
    }
    return OpenedLink{.link = std::move(link), .where = where, .kind = PortKind::Other, .mode = LinkMode::Wifi};
}

OpenFailure failureOf(BleFailure failure) noexcept
{
    switch (failure) {
    case BleFailure::NoRadio:
        return OpenFailure::NoBluetooth;
    case BleFailure::NotPaired:
    case BleFailure::Refused:
        return OpenFailure::NotPaired;
    case BleFailure::NotFound:
    case BleFailure::Unreachable:
        return OpenFailure::NoBoard;
    case BleFailure::None:
    case BleFailure::NoService:
        break;
    }
    return OpenFailure::NoAnswer;
}

void addKey(std::vector<BoardKey>& keys, const BoardKey& key)
{
    if (std::ranges::find(keys, key) == keys.end()) {
        keys.push_back(key);
    }
}

Expected<OpenedLink, OpenFailure> openBluetooth(const Esp32Settings& settings, const LinkEvents& events,
                                                const std::stop_token& stop)
{
    if (settings.radio == nullptr) {
        return makeUnexpected(OpenFailure::NoBluetooth);
    }
    BleRadio& radio = *settings.radio;
    const BluetoothState state = radio.state();
    if (state == BluetoothState::Missing || state == BluetoothState::Off) {
        return makeUnexpected(OpenFailure::NoBluetooth);
    }
    std::optional<std::uint64_t> target;
    std::vector<BoardKey> keys;
    if (!settings.address.empty()) {
        target = parseBluetooth(settings.address);
        if (!target) {
            return makeUnexpected(OpenFailure::BadAddress);
        }
        if (const std::optional<KnownBoard> board = findBluetooth(settings.knownBoards, settings.address)) {
            addKey(keys, board->key);
        }
        for (const KnownBoard& board : settings.knownBoards) {
            addKey(keys, board.key);
        }
        if (keys.empty()) {
            return makeUnexpected(OpenFailure::NoKey);
        }
        if (!radio.linked(*target) && !radio.find(*target, kFindTime, stop)) {
            return makeUnexpected(stop.stop_requested() ? OpenFailure::Stopped : OpenFailure::NoBoard);
        }
    } else {
        const std::vector<SeenBoard> nearby = radio.seen(kSeenRecently);
        bool paired = false;
        for (const KnownBoard& board : settings.knownBoards) {
            const std::optional<std::uint64_t> address = parseBluetooth(board.bluetooth);
            if (!address) {
                continue;
            }
            paired = true;
            const bool inReach = std::ranges::any_of(nearby, [&address](const SeenBoard& seen) {
                return seen.address == *address;
            });
            if (inReach || radio.linked(*address) || radio.find(*address, kFindTime, stop)) {
                target = address;
                addKey(keys, board.key);
                break;
            }
            if (stop.stop_requested()) {
                return makeUnexpected(OpenFailure::Stopped);
            }
        }
        if (!target) {
            return makeUnexpected(paired ? OpenFailure::NoBoard : OpenFailure::NotPaired);
        }
    }
    const std::uint64_t address = *target;
    BleFailure failure = BleFailure::None;
    std::unique_ptr<BlePort> port = BlePort::open(
        [&radio, address](BleChannelEvents channelEvents, BleFailure& channelFailure) {
            return radio.open(address, std::move(channelEvents), channelFailure);
        },
        std::move(keys), failure);
    if (port == nullptr) {
        return makeUnexpected(failureOf(failure));
    }
    LinkOptions options;
    options.allowLegacy = false;
    options.measure = true;
    options.udpGiveUp = kBluetoothGiveUp;
    auto link = std::make_unique<Esp32Link>(std::move(port), options, events);
    if (settle(*link, options.udpGiveUp + kSettleMargin, stop) != LinkState::Ready) {
        return makeUnexpected(stop.stop_requested() ? OpenFailure::Stopped : OpenFailure::NoAnswer);
    }
    return OpenedLink{.link = std::move(link),
                      .where = bluetoothText(address),
                      .kind = PortKind::Other,
                      .mode = LinkMode::Bluetooth};
}

int clampStep(int value, int most) noexcept
{
    return std::clamp(value, -most, most);
}

}

LinkMode linkModeOf(std::string_view name) noexcept
{
    if (name == "wifi") {
        return LinkMode::Wifi;
    }
    return name == "ble" ? LinkMode::Bluetooth : LinkMode::Serial;
}

std::string_view linkModeName(LinkMode mode) noexcept
{
    switch (mode) {
    case LinkMode::Wifi:
        return "wifi";
    case LinkMode::Bluetooth:
        return "ble";
    case LinkMode::Serial:
        break;
    }
    return "serial";
}

Expected<OpenedLink, OpenFailure> openBoard(const Esp32Settings& settings, LinkEvents events,
                                            const std::stop_token& stop)
{
    if (settings.link == LinkMode::Wifi) {
        return openWifi(settings, events, stop);
    }
    if (settings.link == LinkMode::Bluetooth) {
        return openBluetooth(settings, events, stop);
    }
    return openSerial(settings, events, stop);
}

Esp32TouchMethod::Esp32TouchMethod() = default;

Esp32TouchMethod::~Esp32TouchMethod()
{
    close();
}

Expected<void, OpenFailure> Esp32TouchMethod::open(const Esp32Settings& settings, const std::stop_token& stop)
{
    close();
    Expected<OpenedLink, OpenFailure> result = openBoard(settings, {}, stop);
    if (!result) {
        return makeUnexpected(result.error());
    }
    log::info(kCategory, "ESP32 touch method on {}", result->where);
    attach(std::move(result->link));
    return {};
}

void Esp32TouchMethod::attach(std::shared_ptr<Esp32Link> link)
{
    const std::lock_guard lock(m_mutex);
    m_link = std::move(link);
    m_slots.reset();
    m_keyboard.clear();
    m_consumer = 0;
    m_buttons = 0;
    m_gamepad = {};
}

Esp32Capabilities Esp32TouchMethod::capabilities() const
{
    const std::lock_guard lock(m_mutex);
    const bool full = m_link && m_link->state() == LinkState::Ready;
    return Esp32Capabilities{.maxContacts = kMaxContacts, .keys = full, .cancel = false, .mouse = full, .gamepad = full};
}

bool Esp32TouchMethod::frame(std::span<const Contact> contacts, int rotation)
{
    const std::lock_guard lock(m_mutex);
    if (!m_link) {
        return false;
    }
    return m_slots.frame(contacts, rotation, [this](const gs_esp32_touch& touch) { return m_link->sendTouch(touch); });
}

bool Esp32TouchMethod::sendKeyboardLocked()
{
    return m_link->sendKeyboard(m_keyboard.report());
}

bool Esp32TouchMethod::key(std::uint32_t androidKeycode, bool down)
{
    const std::lock_guard lock(m_mutex);
    if (!m_link || m_link->state() != LinkState::Ready) {
        return false;
    }
    const std::optional<HidUsage> usage = hidUsageOf(androidKeycode);
    if (!usage) {
        return false;
    }
    if (usage->kind == UsageKind::Consumer) {
        if (down) {
            m_consumer = usage->usage;
            return m_link->sendConsumer(m_consumer);
        }
        if (m_consumer != usage->usage) {
            return true;
        }
        m_consumer = 0;
        return m_link->sendConsumer(0);
    }
    if (!m_keyboard.apply(*usage, down)) {
        return true;
    }
    return sendKeyboardLocked();
}

bool Esp32TouchMethod::text(std::string_view ascii)
{
    const std::lock_guard lock(m_mutex);
    if (!m_link || m_link->state() != LinkState::Ready) {
        return false;
    }
    bool sent = true;
    for (const char character : ascii) {
        const std::optional<TypedKey> typed = typedKeyOf(character);
        if (!typed) {
            continue;
        }
        gs_esp32_keyboard press = m_keyboard.report();
        if (typed->shift) {
            press.modifiers = static_cast<std::uint8_t>(press.modifiers | kLeftShiftBit);
        }
        const auto slot = std::ranges::find(press.keys, std::uint8_t{0});
        if (slot == std::end(press.keys)) {
            return false;
        }
        *slot = typed->usage;
        sent = m_link->sendKeyboard(press) && sent;
        sent = sendKeyboardLocked() && sent;
    }
    return sent;
}

bool Esp32TouchMethod::mouse(std::uint8_t buttons, int dx, int dy, int wheel)
{
    const std::lock_guard lock(m_mutex);
    if (!m_link || m_link->state() != LinkState::Ready) {
        return false;
    }
    m_buttons = buttons;
    bool sent = true;
    do {
        const int stepX = clampStep(dx, kMostMouseStep);
        const int stepY = clampStep(dy, kMostMouseStep);
        const int stepWheel = clampStep(wheel, kMostWheel);
        const gs_esp32_mouse report{.buttons = buttons,
                                    .dx = static_cast<std::int16_t>(stepX),
                                    .dy = static_cast<std::int16_t>(stepY),
                                    .wheel = static_cast<std::int8_t>(stepWheel)};
        sent = m_link->sendMouse(report) && sent;
        dx -= stepX;
        dy -= stepY;
        wheel -= stepWheel;
    } while (sent && (dx != 0 || dy != 0 || wheel != 0));
    return sent;
}

bool Esp32TouchMethod::gamepad(const Esp32Gamepad& pad)
{
    const std::lock_guard lock(m_mutex);
    if (!m_link || m_link->state() != LinkState::Ready) {
        return false;
    }
    const auto axis = [](std::int16_t value) { return static_cast<std::int16_t>(std::max<int>(value, -32767)); };
    const gs_esp32_gamepad report{.buttons = pad.buttons,
                                  .hat = std::min<std::uint8_t>(pad.hat, GS_ESP32_HAT_CENTERED),
                                  .lx = axis(pad.lx),
                                  .ly = axis(pad.ly),
                                  .rx = axis(pad.rx),
                                  .ry = axis(pad.ry),
                                  .lt = pad.lt,
                                  .rt = pad.rt};
    if (!m_link->sendGamepad(report)) {
        return false;
    }
    m_gamepad = pad;
    return true;
}

void Esp32TouchMethod::releaseLocked()
{
    if (!m_link) {
        return;
    }
    (void)m_slots.release([this](const gs_esp32_touch& touch) { return m_link->sendTouch(touch); });
    if (m_link->state() == LinkState::Ready) {
        (void)m_link->releaseAll();
    }
    m_slots.reset();
    m_keyboard.clear();
    m_consumer = 0;
    m_buttons = 0;
    m_gamepad = {};
}

void Esp32TouchMethod::close()
{
    std::shared_ptr<Esp32Link> link;
    {
        const std::lock_guard lock(m_mutex);
        releaseLocked();
        link = std::move(m_link);
    }
    if (link) {
        link->close();
    }
}

bool Esp32TouchMethod::opened() const
{
    const std::lock_guard lock(m_mutex);
    return m_link != nullptr;
}

std::optional<LinkState> Esp32TouchMethod::state() const
{
    const std::lock_guard lock(m_mutex);
    if (!m_link) {
        return std::nullopt;
    }
    return m_link->state();
}

int Esp32TouchMethod::ignoredContacts() const
{
    const std::lock_guard lock(m_mutex);
    return m_slots.ignored();
}

}
