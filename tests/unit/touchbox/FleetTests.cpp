#include "esp32/FakeBoard.h"
#include "esp32/TestSupport.h"

#include <gs/touchbox/Fleet.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace esp32 = gs::esp32;
using esp32test::eventually;
using esp32test::FakeBoard;
using gs::touchbox::Board;
using gs::touchbox::Candidate;
using gs::touchbox::Fleet;
using gs::touchbox::FleetHooks;
using gs::touchbox::Hold;
using gs::touchbox::Holder;
using gs::touchbox::Probe;
using gs::touchbox::Probed;
using gs::touchbox::Taken;
using namespace std::chrono_literals;

namespace {

esp32::SerialPortInfo portOf(const std::string& name, esp32::PortKind kind)
{
    return esp32::SerialPortInfo{.name = name, .description = "test port", .vendorId = 0, .productId = 0, .kind = kind};
}

esp32::Esp32Settings automatic()
{
    esp32::Esp32Settings settings;
    settings.port = std::string(esp32::kAutomaticPort);
    settings.link = esp32::LinkMode::Serial;
    return settings;
}

std::vector<std::string> placesOf(const std::vector<Candidate>& candidates)
{
    std::vector<std::string> places;
    for (const Candidate& candidate : candidates) {
        places.push_back(candidate.where);
    }
    return places;
}

class BoardPlug final : public esp32::Port
{
public:
    BoardPlug(std::shared_ptr<FakeBoard> board, std::shared_ptr<std::atomic<bool>> unplugged)
        : m_board(std::move(board))
        , m_unplugged(std::move(unplugged))
    {
    }

    [[nodiscard]] bool write(std::span<const std::uint8_t> bytes) override
    {
        return !m_unplugged->load() && m_board->write(bytes);
    }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout) override
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            if (m_interrupted.load() || m_unplugged->load()) {
                return 0;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            const std::size_t got = m_board->read(buffer, std::clamp(left, 0ms, 10ms));
            if (got > 0) {
                return got;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return 0;
    }

    [[nodiscard]] bool setBaud(std::uint32_t baud) override { return m_board->setBaud(baud); }
    [[nodiscard]] bool setDtr(bool) override { return true; }
    [[nodiscard]] bool setRts(bool) override { return true; }
    void discardInput() override { m_board->discardInput(); }
    void interrupt() override { m_interrupted.store(true); }
    [[nodiscard]] bool datagrams() const override { return false; }
    [[nodiscard]] bool gone() const override { return m_unplugged->load(); }

private:
    std::shared_ptr<FakeBoard> m_board;
    std::shared_ptr<std::atomic<bool>> m_unplugged;
    std::atomic<bool> m_interrupted{false};
};

class Bench
{
public:
    void plug(const std::string& name, esp32::PortKind kind, bool answers)
    {
        const std::scoped_lock lock(m_mutex);
        m_ports.push_back(portOf(name, kind));
        if (answers) {
            m_boards[name] = std::make_shared<FakeBoard>(FakeBoard::Options{});
            m_plugs[name] = std::make_shared<std::atomic<bool>>(false);
        }
    }

    void plugSaying(const std::string& name, esp32::PortKind kind, esp32test::Bytes says)
    {
        const std::scoped_lock lock(m_mutex);
        m_ports.push_back(portOf(name, kind));
        FakeBoard::Options options;
        options.answers = false;
        options.says = std::move(says);
        m_boards[name] = std::make_shared<FakeBoard>(options);
        m_plugs[name] = std::make_shared<std::atomic<bool>>(false);
    }

    void unplug(const std::string& name)
    {
        const std::scoped_lock lock(m_mutex);
        std::erase_if(m_ports, [&name](const esp32::SerialPortInfo& port) { return port.name == name; });
        if (const auto plugged = m_plugs.find(name); plugged != m_plugs.end()) {
            plugged->second->store(true);
            m_plugs.erase(plugged);
        }
        m_boards.erase(name);
    }

    void refuse(const std::string& resource)
    {
        const std::scoped_lock lock(m_mutex);
        m_refused.insert(resource);
    }

    void allow(const std::string& resource)
    {
        const std::scoped_lock lock(m_mutex);
        m_refused.erase(resource);
    }

    [[nodiscard]] std::shared_ptr<FakeBoard> board(const std::string& name)
    {
        const std::scoped_lock lock(m_mutex);
        const auto found = m_boards.find(name);
        return found != m_boards.end() ? found->second : nullptr;
    }

    [[nodiscard]] int probesOf(const std::string& name)
    {
        const std::scoped_lock lock(m_mutex);
        return static_cast<int>(std::ranges::count(m_probed, name));
    }

    [[nodiscard]] std::set<std::string> held()
    {
        const std::scoped_lock lock(m_mutex);
        return m_held;
    }

    [[nodiscard]] FleetHooks hooks()
    {
        FleetHooks hooks;
        hooks.watchUsb = false;
        hooks.tapHold = 20ms;
        hooks.list = [this] {
            const std::scoped_lock lock(m_mutex);
            return m_ports;
        };
        hooks.probe = [this](const Candidate& candidate, const esp32::Esp32Settings&, esp32::LinkEvents events,
                             const std::stop_token& stop, esp32::BoardTrouble& trouble) -> Probed {
            std::shared_ptr<FakeBoard> fake;
            std::shared_ptr<std::atomic<bool>> unplugged;
            {
                const std::scoped_lock lock(m_mutex);
                m_probed.push_back(candidate.where);
                if (const auto found = m_boards.find(candidate.where); found != m_boards.end()) {
                    fake = found->second;
                    unplugged = m_plugs[candidate.where];
                }
            }
            if (fake == nullptr) {
                trouble.problem = esp32::BoardProblem::Silent;
                return gs::makeUnexpected(esp32::OpenFailure::NoAnswer);
            }
            esp32::LinkOptions options;
            options.allowLegacy = false;
            auto link = std::make_unique<esp32::Esp32Link>(std::make_unique<BoardPlug>(fake, unplugged), options,
                                                           std::move(events));
            esp32::LinkState state = link->state();
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (state == esp32::LinkState::Opening && !stop.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                state = link->waitSettled(50ms);
            }
            if (state != esp32::LinkState::Ready) {
                trouble = link->trouble();
                return gs::makeUnexpected(stop.stop_requested() ? esp32::OpenFailure::Stopped
                                                                : esp32::OpenFailure::NoAnswer);
            }
            return Probe{.link = std::move(link), .where = candidate.where, .wifi = false, .kind = candidate.kind};
        };
        hooks.claim = [this](const std::string& resource, bool) {
            const std::scoped_lock lock(m_mutex);
            if (m_refused.contains(resource)) {
                return false;
            }
            m_held.insert(resource);
            return true;
        };
        hooks.release = [this](const std::string& resource) {
            const std::scoped_lock lock(m_mutex);
            m_held.erase(resource);
        };
        return hooks;
    }

private:
    std::mutex m_mutex;
    std::vector<esp32::SerialPortInfo> m_ports;
    std::map<std::string, std::shared_ptr<FakeBoard>> m_boards;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_plugs;
    std::vector<std::string> m_probed;
    std::set<std::string> m_refused;
    std::set<std::string> m_held;
};

void lookFor(Fleet& fleet, const esp32::Esp32Settings& settings)
{
    fleet.setSettings(settings);
    fleet.start();
    fleet.setActive(true);
}

bool idle(const Fleet& fleet)
{
    return eventually([&fleet] { return !fleet.busy(); }, 8000ms);
}

bool connected(const Fleet& fleet, const std::string& id)
{
    return eventually([&fleet, &id] { return fleet.link(id) != nullptr; }, 8000ms);
}

}

TEST_CASE("ADR-0112: a board is named by its place, and a hold covers it by its serial port or by Wi-Fi")
{
    CHECK(gs::touchbox::boardIdOf("COM5", false) == "serial-com5");
    CHECK(gs::touchbox::boardIdOf("192.168.1.20:4545", true) == "wifi-192-168-1-20-4545");
    CHECK(gs::touchbox::boardIdOf(std::string(80, 'a'), true).size() == 64);
    CHECK(gs::touchbox::serialResource("com5") == "serial:COM5");
    CHECK(gs::touchbox::sameResource("serial:com5", "SERIAL:COM5"));
    CHECK_FALSE(gs::touchbox::sameResource("serial:COM5", "serial:COM15"));
    CHECK(gs::touchbox::samePort("COM5", "com5"));
    CHECK_FALSE(gs::touchbox::samePort("COM5", "COM15"));
    CHECK_FALSE(gs::touchbox::samePort("", ""));

    const Hold every = gs::touchbox::holdOf(automatic());
    CHECK(every == Hold{.where = "", .wifi = false, .every = true});
    CHECK(gs::touchbox::covers(every, "COM3", false));
    CHECK_FALSE(gs::touchbox::covers(every, "192.168.1.20:4545", true));

    esp32::Esp32Settings chosen = automatic();
    chosen.port = "COM5";
    const Hold one = gs::touchbox::holdOf(chosen);
    CHECK(one == Hold{.where = "COM5"});
    CHECK(gs::touchbox::covers(one, "com5", false));
    CHECK_FALSE(gs::touchbox::covers(one, "COM6", false));

    esp32::Esp32Settings wifi = automatic();
    wifi.link = esp32::LinkMode::Wifi;
    wifi.address = "esp32.local";
    const Hold air = gs::touchbox::holdOf(wifi);
    CHECK(air.wifi);
    CHECK(gs::touchbox::covers(air, "192.168.1.20:4545", true));
    CHECK_FALSE(gs::touchbox::covers(air, "COM5", false));
}

TEST_CASE("ADR-0112: the boards looked for are the ESP32-like ports, the chosen port or the Wi-Fi address, minus those "
          "in use")
{
    const std::vector<esp32::SerialPortInfo> ports{portOf("COM3", esp32::PortKind::Other),
                                                   portOf("COM5", esp32::PortKind::EspressifUsb),
                                                   portOf("COM7", esp32::PortKind::Ch34x),
                                                   portOf("COM8", esp32::PortKind::Cp210x)};
    const std::vector<std::string> likely{"COM5", "COM7", "COM8"};
    const std::vector<Candidate> all = gs::touchbox::candidatesOf(automatic(), ports, {});
    CHECK(placesOf(all) == likely);
    REQUIRE(all.size() == 3);
    CHECK(all[1].kind == esp32::PortKind::Ch34x);
    CHECK_FALSE(all[1].chosen);

    const std::vector<std::string> free{"COM5", "COM8"};
    CHECK(placesOf(gs::touchbox::candidatesOf(automatic(), ports, {Hold{.where = "COM7"}})) == free);
    CHECK(gs::touchbox::candidatesOf(automatic(), ports, {gs::touchbox::holdOf(automatic())}).empty());

    esp32::Esp32Settings chosen = automatic();
    chosen.port = "COM9";
    const std::vector<Candidate> unlisted = gs::touchbox::candidatesOf(chosen, ports, {});
    REQUIRE(unlisted.size() == 1);
    CHECK(unlisted[0] == Candidate{.where = "COM9", .wifi = false, .kind = esp32::PortKind::Other, .chosen = true});
    chosen.port = "COM7";
    const std::vector<Candidate> listed = gs::touchbox::candidatesOf(chosen, ports, {});
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].kind == esp32::PortKind::Ch34x);
    chosen.port = "USB1";
    CHECK(gs::touchbox::candidatesOf(chosen, ports, {}).empty());

    esp32::Esp32Settings wifi = automatic();
    wifi.link = esp32::LinkMode::Wifi;
    wifi.address = "192.168.1.20:4545";
    const std::vector<Candidate> remote = gs::touchbox::candidatesOf(wifi, ports, {});
    REQUIRE(remote.size() == 1);
    CHECK(remote[0] ==
          Candidate{.where = "192.168.1.20:4545", .wifi = true, .kind = esp32::PortKind::Other, .chosen = true});
    CHECK(gs::touchbox::candidatesOf(wifi, ports, {Hold{.where = "10.0.0.9:4545", .wifi = true}}).empty());
    CHECK(gs::touchbox::candidatesOf(wifi, ports, {Hold{.where = "COM5"}}).size() == 1);
}

TEST_CASE("ADR-0112: a board that answers on an ESP32-like port is found and claimed, its phone's state follows and "
          "unplugging removes it")
{
    Bench bench;
    bench.plug("COM3", esp32::PortKind::Other, false);
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    std::atomic<int> changes{0};
    Fleet fleet(bench.hooks(), [&changes] { ++changes; });
    lookFor(fleet, automatic());

    REQUIRE(connected(fleet, "serial-com7"));
    REQUIRE(fleet.boards().size() == 1);
    const Board found = fleet.boards().front();
    CHECK(found.id == "serial-com7");
    CHECK(found.where == "COM7");
    CHECK_FALSE(found.wifi);
    CHECK(found.connected);
    CHECK_FALSE(found.held);
    REQUIRE(found.info.has_value());
    CHECK(found.info->chipId == GS_ESP32_CHIP_ESP32S3);
    CHECK(found.info->firmware == "1.0.0");
    CHECK(bench.probesOf("COM3") == 0);
    CHECK(fleet.matching(automatic()).has_value());
    CHECK(changes.load() > 0);
    CHECK(bench.held() == std::set<std::string>{"serial:COM7"});

    bench.board("COM7")->pushStatus(GS_ESP32_USB_READY, GS_ESP32_BLE_NONE, "");
    CHECK(eventually([&fleet] {
        const std::optional<Board> board = fleet.board("serial-com7");
        return board.has_value() && board->status.usb == GS_ESP32_USB_READY;
    }));

    bench.unplug("COM7");
    CHECK(eventually([&fleet] { return fleet.boards().empty(); }));
    CHECK(eventually([&bench] { return bench.held().empty(); }));
}

TEST_CASE("ADR-0112: a session takes the board's link together with its claim, and the board is found again when the "
          "session lets go")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));
    REQUIRE(idle(fleet));

    Taken taken = fleet.take("serial-com7", "session:1", Holder::Session);
    REQUIRE(taken.link != nullptr);
    CHECK(taken.resource == "serial:COM7");
    CHECK(fleet.link("serial-com7") == nullptr);
    const std::optional<Board> held = fleet.board("serial-com7");
    REQUIRE(held.has_value());
    CHECK(held->held);
    CHECK(held->inSession);
    CHECK_FALSE(held->connected);
    CHECK(held->info.has_value());
    CHECK(fleet.inSession("COM7", false));
    CHECK(fleet.take("serial-com7", "session:2", Holder::Session).link == nullptr);

    const int probes = bench.probesOf("COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM7") == probes);
    CHECK(bench.held() == std::set<std::string>{"serial:COM7"});

    taken.link.reset();
    fleet.release("session:1");
    fleet.unacquire(taken.resource);
    REQUIRE(connected(fleet, "serial-com7"));
    CHECK(bench.probesOf("COM7") == probes + 1);
    CHECK(bench.held() == std::set<std::string>{"serial:COM7"});
    CHECK_FALSE(fleet.inSession("COM7", false));
}

TEST_CASE("ADR-0112: a hold closes the board's port and keeps it out of the scans until it is released")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));

    fleet.hold("flash:1", Hold{.where = "COM7", .holder = Holder::Flash});
    CHECK(fleet.link("serial-com7") == nullptr);
    CHECK(fleet.held("COM7", false));
    CHECK_FALSE(fleet.inSession("COM7", false));
    const std::optional<Board> flashing = fleet.board("serial-com7");
    REQUIRE(flashing.has_value());
    CHECK(flashing->flashing);
    CHECK(flashing->held);
    CHECK(eventually([&bench] { return bench.held().empty(); }));

    const int probes = bench.probesOf("COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM7") == probes);

    fleet.release("flash:1");
    REQUIRE(connected(fleet, "serial-com7"));
    CHECK(bench.probesOf("COM7") == probes + 1);

    fleet.hold("phone", gs::touchbox::holdOf(automatic()));
    CHECK(fleet.link("serial-com7") == nullptr);
    CHECK(fleet.inSession("COM7", false));
    fleet.release("phone");
    CHECK(connected(fleet, "serial-com7"));
}

TEST_CASE("ADR-0112: an ESP32-like port that does not answer twice is left alone until it is plugged in again")
{
    Bench bench;
    bench.plug("COM9", esp32::PortKind::Ch34x, false);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(eventually([&bench, &fleet] { return bench.probesOf("COM9") == 1 && !fleet.busy(); }, 8000ms));
    REQUIRE(fleet.problem().has_value());
    CHECK(*fleet.problem() == esp32::OpenFailure::NoBoard);

    fleet.rescan();
    REQUIRE(eventually([&bench, &fleet] { return bench.probesOf("COM9") == 2 && !fleet.busy(); }, 8000ms));
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM9") == 2);

    bench.unplug("COM9");
    fleet.rescan();
    REQUIRE(idle(fleet));
    bench.plug("COM9", esp32::PortKind::Ch34x, false);
    fleet.rescan();
    REQUIRE(eventually([&bench, &fleet] { return bench.probesOf("COM9") == 3 && !fleet.busy(); }, 8000ms));
    CHECK(fleet.boards().empty());
    CHECK(bench.held().empty());
}

TEST_CASE("ADR-0112: disconnecting a board lets go of it until its port goes away or the boards are looked for again")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));

    fleet.dismiss("serial-com7");
    CHECK(fleet.boards().empty());
    const int probes = bench.probesOf("COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM7") == probes);
    CHECK(fleet.boards().empty());
    CHECK(bench.held().empty());

    fleet.forgive();
    REQUIRE(connected(fleet, "serial-com7"));

    fleet.dismiss("serial-com7");
    bench.unplug("COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    fleet.rescan();
    CHECK(connected(fleet, "serial-com7"));
}

TEST_CASE("ADR-0112: changing the settings starts over and turning the fleet off forgets the boards")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));

    esp32::Esp32Settings chosen = automatic();
    chosen.port = "COM8";
    fleet.setSettings(chosen);
    CHECK(fleet.boards().empty());
    REQUIRE(eventually([&bench, &fleet] { return bench.probesOf("COM8") == 1 && !fleet.busy(); }, 8000ms));
    CHECK(fleet.boards().empty());
    REQUIRE(fleet.problem().has_value());
    CHECK(*fleet.problem() == esp32::OpenFailure::NoAnswer);

    esp32::Esp32Settings same = chosen;
    same.knownBoards.push_back(esp32::KnownBoard{.id = "a1b2c3d4e5f6"});
    const int probes = bench.probesOf("COM8");
    fleet.setSettings(same);
    CHECK(fleet.settings().knownBoards.size() == 1);
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM8") == probes);

    fleet.setSettings(automatic());
    REQUIRE(connected(fleet, "serial-com7"));
    fleet.setActive(false);
    CHECK_FALSE(fleet.active());
    CHECK(fleet.boards().empty());
    REQUIRE(idle(fleet));
    const int before = bench.probesOf("COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM7") == before);
    CHECK(eventually([&bench] { return bench.held().empty(); }));
}

TEST_CASE("ADR-0112: a port someone else holds is not opened, and an idle board is let go to whoever asks for its "
          "port unless a session uses it")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    bench.plug("COM8", esp32::PortKind::Cp210x, true);
    bench.refuse("serial:COM8");
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));
    REQUIRE(idle(fleet));
    CHECK(bench.probesOf("COM8") == 0);
    CHECK_FALSE(fleet.board("serial-com8").has_value());
    CHECK(bench.held() == std::set<std::string>{"serial:COM7"});

    CHECK(fleet.yield("serial:com7", "Your logic"));
    CHECK(fleet.link("serial-com7") == nullptr);
    const std::optional<Board> yielded = fleet.board("serial-com7");
    REQUIRE(yielded.has_value());
    CHECK(yielded->yieldedTo == "Your logic");
    CHECK(eventually([&bench] { return bench.held().empty(); }));

    bench.refuse("serial:COM7");
    fleet.rescan();
    REQUIRE(idle(fleet));
    CHECK(fleet.board("serial-com7").has_value());
    CHECK(fleet.link("serial-com7") == nullptr);

    bench.allow("serial:COM7");
    fleet.rescan();
    REQUIRE(connected(fleet, "serial-com7"));
    CHECK(fleet.board("serial-com7")->yieldedTo.empty());

    Taken taken = fleet.take("serial-com7", "session:1", Holder::Session);
    REQUIRE(taken.link != nullptr);
    CHECK_FALSE(fleet.yield("serial:COM7", "Your logic"));
    CHECK(fleet.yield("ble:AA:BB:CC:DD:EE:FF", "Your logic"));
    taken.link.reset();
    fleet.release("session:1");
    fleet.unacquire(taken.resource);
}

TEST_CASE("ADR-0112: a tap presses the middle of the phone's screen and lifts the finger a moment later")
{
    Bench bench;
    bench.plug("COM7", esp32::PortKind::EspressifUsb, true);
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(connected(fleet, "serial-com7"));
    const std::shared_ptr<FakeBoard> board = bench.board("COM7");
    const std::size_t before = board->count(GS_ESP32_MSG_TOUCH);

    REQUIRE(fleet.tap("serial-com7"));
    REQUIRE(eventually([&board, before] { return board->count(GS_ESP32_MSG_TOUCH) >= before + 2; }));
    const auto touches = board->framesOf(GS_ESP32_MSG_TOUCH);
    gs_esp32_touch pressed{};
    gs_esp32_touch lifted{};
    REQUIRE(gs_esp32_decode_touch(touches[before].payload.data(), touches[before].payload.size(), &pressed));
    REQUIRE(gs_esp32_decode_touch(touches[before + 1].payload.data(), touches[before + 1].payload.size(), &lifted));
    CHECK(pressed.count == 1);
    CHECK(pressed.contacts[0].tip == 1);
    CHECK(pressed.contacts[0].x == 16384);
    CHECK(pressed.contacts[0].y == 16384);
    CHECK(lifted.contacts[0].tip == 0);
    CHECK_FALSE(fleet.tap("serial-com99"));
}

TEST_CASE("ADR-0112: a board that keeps restarting is reported as such, not as old firmware")
{
    Bench bench;
    bench.plugSaying("COM9", esp32::PortKind::Ch34x,
                     esp32test::textBytes("ESP-ROM:esp32s3-20210327\r\n"
                                          "rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"));
    Fleet fleet(bench.hooks());
    lookFor(fleet, automatic());
    REQUIRE(eventually([&fleet] { return fleet.trouble().has_value() && !fleet.busy(); }, 8000ms));
    CHECK(fleet.trouble()->problem == esp32::BoardProblem::Restarting);
    CHECK(fleet.trouble()->resetReason == "0x3 (RTC_SW_SYS_RST)");
    CHECK(fleet.boards().empty());
}
