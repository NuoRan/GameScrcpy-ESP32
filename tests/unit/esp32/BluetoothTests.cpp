#include "FakeBluetooth.h"
#include "TestSupport.h"

#include <gs/esp32/BlePort.h>
#include <gs/esp32/Bluetooth.h>
#include <gs/esp32/Link.h>
#include <gs/esp32/TouchMethod.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace esp32 = gs::esp32;
using esp32test::Bytes;
using esp32test::eventually;
using esp32test::FakeBleBoard;
using esp32test::FakeBleChannel;
using esp32test::FakeRadio;
using esp32test::fromHex;
using namespace std::chrono_literals;

namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kAddress = 0xDEADBEEF0104ULL;

const Json& vectors()
{
    static const Json document = [] {
        std::ifstream file(GS_ESP32_VECTORS, std::ios::binary);
        REQUIRE(file.good());
        return Json::parse(file);
    }();
    return document;
}

esp32::BoardKey keyOf(std::uint8_t seed)
{
    esp32::BoardKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(seed + i);
    }
    return key;
}

std::shared_ptr<FakeBleBoard> boardWith(const esp32::BoardKey& key, std::size_t chunk)
{
    FakeBleBoard::Options options;
    options.key = key;
    options.chunk = chunk;
    return std::make_shared<FakeBleBoard>(options);
}

esp32::BleConnect connectTo(const std::shared_ptr<FakeBleBoard>& board,
                            const std::shared_ptr<std::atomic<bool>>& held = nullptr,
                            FakeBleChannel** channel = nullptr)
{
    return [board, held, channel](esp32::BleChannelEvents events,
                                  esp32::BleFailure& failure) -> std::unique_ptr<esp32::BleChannel> {
        board->attach(std::move(events));
        failure = esp32::BleFailure::None;
        auto made = std::make_unique<FakeBleChannel>(board, held);
        if (channel != nullptr) {
            *channel = made.get();
        }
        return made;
    };
}

Bytes frameOf(std::uint8_t type, const Bytes& payload)
{
    std::array<std::uint8_t, GS_ESP32_MAX_FRAME> frame{};
    const std::size_t size = gs_esp32_frame_encode(frame.data(), frame.size(), type, 0, payload.data(), payload.size());
    return Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(size));
}

Bytes helloRequest()
{
    return frameOf(GS_ESP32_MSG_HELLO_REQ, Bytes{GS_ESP32_PROTOCOL_VERSION});
}

Bytes pingOf(std::uint32_t token)
{
    std::array<std::uint8_t, 4> payload{};
    REQUIRE(gs_esp32_encode_token(payload.data(), payload.size(), token) == 4);
    return frameOf(GS_ESP32_MSG_PING, Bytes(payload.begin(), payload.end()));
}

esp32::OpenFailure refusal(const esp32::Esp32Settings& settings)
{
    auto opened = esp32::openBoard(settings);
    REQUIRE_FALSE(opened.has_value());
    return opened.error();
}

}

TEST_CASE("ADR-0112: a Bluetooth address is written as AA:BB:CC:DD:EE:FF and read with colons, dashes or none")
{
    CHECK(esp32::bluetoothText(kAddress) == "DE:AD:BE:EF:01:04");
    CHECK(esp32::parseBluetooth("de:ad:be:ef:01:04") == std::optional<std::uint64_t>{kAddress});
    CHECK(esp32::parseBluetooth(" DE-AD-BE-EF-01-04 ") == std::optional<std::uint64_t>{kAddress});
    CHECK(esp32::parseBluetooth("DEADBEEF0104") == std::optional<std::uint64_t>{kAddress});
    CHECK_FALSE(esp32::parseBluetooth("DE:AD:BE:EF:01").has_value());
    CHECK_FALSE(esp32::parseBluetooth("DE:AD:BE:EF:01:04:05").has_value());
    CHECK_FALSE(esp32::parseBluetooth("D:EAD:BE:EF:01:04").has_value());
    CHECK_FALSE(esp32::parseBluetooth("DE:AD:BE:EF:01:04:").has_value());
    CHECK_FALSE(esp32::parseBluetooth("00:00:00:00:00:00").has_value());
    CHECK_FALSE(esp32::parseBluetooth("COM7").has_value());
    CHECK_FALSE(esp32::parseBluetooth("").has_value());
    CHECK(esp32::bluetoothResource(kAddress) == "ble:DE:AD:BE:EF:01:04");
    CHECK(esp32::sameBluetooth("de:ad:be:ef:01:04", "DEADBEEF0104"));
    CHECK_FALSE(esp32::sameBluetooth("", ""));
    CHECK_FALSE(esp32::sameBluetooth("DE:AD:BE:EF:01:04", "DE:AD:BE:EF:01:05"));
}

TEST_CASE("ADR-0112: the board's id, chip and key flag are read from the serial service's data in its scan response")
{
    REQUIRE_FALSE(vectors().at("bluetooth").at("adverts").empty());
    for (const Json& vector : vectors().at("bluetooth").at("adverts")) {
        INFO(vector.at("name").get<std::string>());
        const std::optional<esp32::BoardAdvert> advert =
            esp32::advertOf(fromHex(vector.at("section").get<std::string>()));
        REQUIRE(advert.has_value());
        const Json& fields = vector.at("fields");
        const int flags = fields.at("flags").get<int>();
        CHECK(advert->id == fields.at("id").get<std::string>());
        CHECK(advert->chipId == fields.at("chip").get<int>());
        CHECK(advert->keyed == ((flags & 0x01) != 0));
        CHECK(advert->busy == ((flags & 0x02) != 0));
    }
    for (const Json& vector : vectors().at("bluetooth").at("invalidAdverts")) {
        INFO(vector.at("name").get<std::string>());
        CHECK_FALSE(esp32::advertOf(fromHex(vector.at("section").get<std::string>())).has_value());
    }
    CHECK(esp32::signalLevel(-50) == esp32::SignalLevel::Good);
    CHECK(esp32::signalLevel(-67) == esp32::SignalLevel::Good);
    CHECK(esp32::signalLevel(-75) == esp32::SignalLevel::Fair);
    CHECK(esp32::signalLevel(-81) == esp32::SignalLevel::Weak);
}

TEST_CASE("ADR-0112: the Bluetooth port seals each frame with the board's key, cuts writes to the channel's size and "
          "opens what comes back")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 20);
    esp32::BleFailure failure = esp32::BleFailure::NoRadio;
    const std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {key}, failure);
    REQUIRE(port != nullptr);
    CHECK(failure == esp32::BleFailure::None);
    CHECK(port->datagrams());
    CHECK_FALSE(port->setBaud(GS_ESP32_FAST_BAUD));
    CHECK_FALSE(port->gone());
    REQUIRE(port->write(helloRequest()));
    std::array<std::uint8_t, GS_ESP32_MAX_DATAGRAM> buffer{};
    const std::size_t size = port->read(buffer, 2000ms);
    REQUIRE(size > 0);
    gs_esp32_frame frame{};
    REQUIRE(gs_esp32_frame_decode(buffer.data(), size, &frame));
    CHECK(frame.type == GS_ESP32_MSG_HELLO);
    CHECK(board->count(GS_ESP32_MSG_HELLO_REQ) == 1);
    CHECK(board->badTags() == 0);
    REQUIRE_FALSE(board->writes().empty());
    for (const std::size_t each : board->writes()) {
        CHECK(each <= 20);
    }
    CHECK(port->matchedKey() == std::optional<esp32::BoardKey>{key});
    CHECK_FALSE(port->write(Bytes{}));
    CHECK_FALSE(port->write(Bytes(GS_ESP32_MAX_FRAME + 1, 0)));
    esp32::BleFailure refused = esp32::BleFailure::None;
    CHECK(esp32::BlePort::open(connectTo(board), {}, refused) == nullptr);
    CHECK(refused == esp32::BleFailure::NotPaired);
}

TEST_CASE("ADR-0112: until an answer shows which key the board has, each frame goes out once per known key")
{
    const esp32::BoardKey right = keyOf(1);
    const esp32::BoardKey wrong = keyOf(100);
    const std::shared_ptr<FakeBleBoard> board = boardWith(right, 244);
    esp32::BleFailure failure = esp32::BleFailure::None;
    const std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {wrong, right}, failure);
    REQUIRE(port != nullptr);
    REQUIRE(port->write(helloRequest()));
    std::array<std::uint8_t, GS_ESP32_MAX_DATAGRAM> buffer{};
    REQUIRE(port->read(buffer, 2000ms) > 0);
    CHECK(board->badTags() == 1);
    CHECK(board->records() == 1);
    CHECK(port->matchedKey() == std::optional<esp32::BoardKey>{right});
    REQUIRE(port->write(helloRequest()));
    REQUIRE(port->read(buffer, 2000ms) > 0);
    CHECK(board->badTags() == 1);
    CHECK(board->records() == 2);
}

TEST_CASE("ADR-0112: a record with a wrong tag from the air is dropped and counted, and the next good one is read")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 7);
    esp32::BleFailure failure = esp32::BleFailure::None;
    const std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {key}, failure);
    REQUIRE(port != nullptr);
    board->push(FakeBleBoard::sealed(GS_ESP32_MSG_HELLO, Bytes{2}, keyOf(9)));
    std::array<std::uint8_t, GS_ESP32_MAX_DATAGRAM> buffer{};
    CHECK(port->read(buffer, 100ms) == 0);
    CHECK(port->droppedRecords() == 1);
    board->push(FakeBleBoard::sealed(GS_ESP32_MSG_LOG, Bytes{2, 2, 'o', 'k'}, key));
    const std::size_t size = port->read(buffer, 2000ms);
    REQUIRE(size > 0);
    gs_esp32_frame frame{};
    REQUIRE(gs_esp32_frame_decode(buffer.data(), size, &frame));
    CHECK(frame.type == GS_ESP32_MSG_LOG);
}

TEST_CASE("ADR-0112: frames written while a write is under way go out together in the next write")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 244);
    const auto held = std::make_shared<std::atomic<bool>>(true);
    FakeBleChannel* channel = nullptr;
    esp32::BleFailure failure = esp32::BleFailure::None;
    const std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board, held, &channel), {key}, failure);
    REQUIRE(port != nullptr);
    REQUIRE(channel != nullptr);
    REQUIRE(port->write(pingOf(1)));
    REQUIRE(eventually([channel] { return channel->waiting(); }));
    REQUIRE(port->write(pingOf(2)));
    REQUIRE(port->write(pingOf(3)));
    held->store(false);
    REQUIRE(eventually([&board] { return board->count(GS_ESP32_MSG_PING) == 3; }));
    const std::size_t record = GS_ESP32_FRAME_OVERHEAD + 4 + GS_ESP32_TAG_SIZE;
    CHECK(board->writes() == std::vector<std::size_t>{record, 2 * record});
}

TEST_CASE("ADR-0112: a link over Bluetooth shakes hands like Wi-Fi, pings all the time and reads the board's signal")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 20);
    esp32::BleFailure failure = esp32::BleFailure::None;
    std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {key}, failure);
    REQUIRE(port != nullptr);
    esp32::LinkOptions options;
    options.allowLegacy = false;
    options.measure = true;
    options.keepAlive = 20ms;
    esp32::Esp32Link link(std::move(port), options);
    REQUIRE(link.waitSettled(3s) == esp32::LinkState::Ready);
    CHECK(link.datagrams());
    REQUIRE(link.board().has_value());
    CHECK((link.board()->features & GS_ESP32_FEATURE_BLE_LINK) != 0);
    CHECK(eventually([&link] { return link.signal() == std::optional<int>{-58}; }));
    CHECK(eventually([&link] { return link.slowRoundTrip().has_value(); }, 5000ms));
    gs_esp32_touch touch{};
    touch.count = 1;
    touch.contacts[0] = {.id = 0, .tip = 1, .x = 100, .y = 200};
    CHECK(link.sendTouch(touch));
    CHECK(eventually([&board] { return board->count(GS_ESP32_MSG_TOUCH) == 1; }));
    CHECK(link.baud() == GS_ESP32_HANDSHAKE_BAUD);
    const std::uint32_t before = link.hellos();
    CHECK(link.requestHello());
    CHECK(link.waitHello(before, 2000ms));
}

TEST_CASE("ADR-0112: before twenty round trips are known there is no slow round trip, and no signal before a PONG")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 244);
    esp32::BleFailure failure = esp32::BleFailure::None;
    std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {key}, failure);
    REQUIRE(port != nullptr);
    esp32::LinkOptions options;
    options.allowLegacy = false;
    options.keepAlive = 10s;
    esp32::Esp32Link link(std::move(port), options);
    REQUIRE(link.waitSettled(3s) == esp32::LinkState::Ready);
    CHECK_FALSE(link.slowRoundTrip().has_value());
    CHECK_FALSE(link.signal().has_value());
}

TEST_CASE("ADR-0112: when the Bluetooth channel is lost the port is gone and the link is lost")
{
    const esp32::BoardKey key = keyOf(1);
    const std::shared_ptr<FakeBleBoard> board = boardWith(key, 20);
    esp32::BleFailure failure = esp32::BleFailure::None;
    std::unique_ptr<esp32::BlePort> port = esp32::BlePort::open(connectTo(board), {key}, failure);
    REQUIRE(port != nullptr);
    const esp32::BlePort* watched = port.get();
    esp32::LinkOptions options;
    options.allowLegacy = false;
    esp32::Esp32Link link(std::move(port), options);
    REQUIRE(link.waitSettled(3s) == esp32::LinkState::Ready);
    board->drop();
    CHECK(eventually([&link] { return link.state() == esp32::LinkState::Lost; }));
    CHECK(watched->gone());
}

TEST_CASE("ADR-0112: opening over Bluetooth needs Bluetooth, a paired board in reach and its key")
{
    const esp32::BoardKey key = keyOf(1);
    auto radio = std::make_shared<FakeRadio>();
    radio->add(kAddress, boardWith(key, 20));
    esp32::Esp32Settings settings;
    settings.link = esp32::LinkMode::Bluetooth;
    CHECK(refusal(settings) == esp32::OpenFailure::NoBluetooth);
    settings.radio = radio;
    radio->setState(esp32::BluetoothState::Off);
    CHECK(refusal(settings) == esp32::OpenFailure::NoBluetooth);
    radio->setState(esp32::BluetoothState::Ready);
    CHECK(refusal(settings) == esp32::OpenFailure::NotPaired);
    settings.address = "not an address";
    CHECK(refusal(settings) == esp32::OpenFailure::BadAddress);
    settings.address = "DE:AD:BE:EF:01:04";
    CHECK(refusal(settings) == esp32::OpenFailure::NoKey);
    settings.address.clear();
    settings.knownBoards.push_back(esp32::KnownBoard{
        .id = "deadbeef0102", .name = "Fake C3", .key = key, .address = {}, .bluetooth = "DE:AD:BE:EF:01:04"});
    auto opened = esp32::openBoard(settings);
    REQUIRE(opened.has_value());
    CHECK(opened->mode == esp32::LinkMode::Bluetooth);
    CHECK(opened->where == "DE:AD:BE:EF:01:04");
    CHECK(opened->link->state() == esp32::LinkState::Ready);
    CHECK(radio->opened() == 1);
    opened->link.reset();

    auto hidden = std::make_shared<FakeRadio>();
    hidden->add(kAddress, boardWith(key, 20), false);
    settings.radio = hidden;
    CHECK(refusal(settings) == esp32::OpenFailure::NoBoard);
    CHECK(esp32::linkModeOf("ble") == esp32::LinkMode::Bluetooth);
    CHECK(esp32::linkModeOf("wifi") == esp32::LinkMode::Wifi);
    CHECK(esp32::linkModeOf("anything") == esp32::LinkMode::Serial);
    CHECK(esp32::linkModeName(esp32::LinkMode::Bluetooth) == "ble");
}
