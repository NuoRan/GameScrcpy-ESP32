#pragma once

#include <gs/esp32/Bluetooth.h>
#include <gs/esp32/Diagnosis.h>
#include <gs/esp32/Link.h>
#include <gs/esp32/Port.h>
#include <gs/esp32/TouchMethod.h>
#include <gs/support/Expected.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gs::platform {
class UsbWatcher;
}

namespace gs::touchbox {

[[nodiscard]] std::string boardIdOf(std::string_view where, esp32::LinkMode via);
[[nodiscard]] std::string serialResource(std::string_view where);
[[nodiscard]] std::string resourceOf(std::string_view where, esp32::LinkMode via);
[[nodiscard]] bool samePort(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool sameResource(std::string_view a, std::string_view b) noexcept;

enum class Holder : std::uint8_t { Session, Flash, Job };

struct Hold {
    std::string where;
    esp32::LinkMode via = esp32::LinkMode::Serial;
    bool every = false;
    Holder holder = Holder::Session;

    friend bool operator==(const Hold&, const Hold&) = default;
};

[[nodiscard]] Hold holdOf(const esp32::Esp32Settings& settings);
[[nodiscard]] bool covers(const Hold& hold, std::string_view where, esp32::LinkMode via) noexcept;

struct Candidate {
    std::string where;
    esp32::LinkMode via = esp32::LinkMode::Serial;
    esp32::PortKind kind = esp32::PortKind::Other;
    bool chosen = false;

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

[[nodiscard]] std::vector<Candidate> candidatesOf(const esp32::Esp32Settings& settings,
                                                  const std::vector<esp32::SerialPortInfo>& ports,
                                                  const std::vector<Hold>& blockers);

struct Probe {
    std::unique_ptr<esp32::Esp32Link> link;
    std::string where;
    esp32::LinkMode via = esp32::LinkMode::Serial;
    esp32::PortKind kind = esp32::PortKind::Other;
};

using Probed = Expected<Probe, esp32::OpenFailure>;

[[nodiscard]] Probed probeBoard(const Candidate& candidate, const esp32::Esp32Settings& settings,
                                esp32::LinkEvents events, const std::stop_token& stop, esp32::BoardTrouble& trouble);

struct FleetHooks {
    std::function<std::vector<esp32::SerialPortInfo>()> list = esp32::listSerialPorts;
    std::function<Probed(const Candidate&, const esp32::Esp32Settings&, esp32::LinkEvents, const std::stop_token&,
                         esp32::BoardTrouble&)>
        probe = probeBoard;
    std::function<bool(const std::string& resource, bool wait)> claim;
    std::function<void(const std::string& resource)> release;
    std::shared_ptr<esp32::BleRadio> radio;
    bool watchUsb = true;
    std::chrono::milliseconds retry{15000};
    std::chrono::milliseconds tapHold{80};
};

struct Board {
    std::string id;
    std::string where;
    esp32::LinkMode via = esp32::LinkMode::Serial;
    esp32::PortKind kind = esp32::PortKind::Other;
    std::optional<esp32::BoardInfo> info;
    esp32::BoardStatus status;
    std::optional<std::chrono::microseconds> roundTrip;
    std::optional<std::chrono::microseconds> slowTrip;
    std::optional<int> signal;
    esp32::BoardTrouble trouble;
    bool legacy = false;
    bool connected = false;
    bool held = false;
    bool inSession = false;
    bool flashing = false;
    std::string yieldedTo;

    friend bool operator==(const Board&, const Board&) = default;
};

inline constexpr std::chrono::milliseconds kSlowBluetooth{30};

[[nodiscard]] bool slowLink(const Board& board) noexcept;
[[nodiscard]] bool sameShape(const Board& a, const Board& b) noexcept;

struct Taken {
    std::shared_ptr<esp32::Esp32Link> link;
    std::string resource;
};

class Fleet
{
public:
    using Changed = std::function<void()>;

    explicit Fleet(FleetHooks hooks = {}, Changed changed = {});
    ~Fleet();
    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    void start();
    void setActive(bool active);
    void setSettings(esp32::Esp32Settings settings);
    void rescan();
    void rescanAfter(std::chrono::milliseconds delay);

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] esp32::Esp32Settings settings() const;
    [[nodiscard]] std::vector<Board> boards() const;
    [[nodiscard]] std::optional<Board> board(std::string_view id) const;
    [[nodiscard]] std::optional<Board> matching(const esp32::Esp32Settings& settings) const;
    [[nodiscard]] std::vector<esp32::SerialPortInfo> ports() const;
    [[nodiscard]] std::optional<esp32::OpenFailure> problem() const;
    [[nodiscard]] std::optional<esp32::BoardTrouble> trouble() const;
    [[nodiscard]] std::shared_ptr<esp32::Esp32Link> link(std::string_view id) const;

    [[nodiscard]] Taken take(std::string_view id, const std::string& owner, Holder holder);
    void hold(const std::string& owner, Hold wanted);
    void release(const std::string& owner, std::chrono::milliseconds delay = std::chrono::milliseconds{0});
    [[nodiscard]] bool held(std::string_view where, esp32::LinkMode via) const;
    [[nodiscard]] bool inSession(std::string_view where, esp32::LinkMode via) const;
    [[nodiscard]] bool acquire(const std::string& resource, bool wait);
    void unacquire(const std::string& resource);
    [[nodiscard]] bool yield(std::string_view resource, std::string_view by);
    void dismiss(std::string_view id);
    void forgive();
    [[nodiscard]] bool tap(std::string_view id);

private:
    using Clock = std::chrono::steady_clock;

    struct Gate {
        std::mutex mutex;
        Fleet* owner = nullptr;
    };

    struct Entry {
        std::string id;
        std::string where;
        esp32::LinkMode via = esp32::LinkMode::Serial;
        esp32::PortKind kind = esp32::PortKind::Other;
        std::optional<esp32::BoardInfo> info;
        esp32::BoardStatus status;
        esp32::BoardTrouble trouble;
        bool legacy = false;
        std::string yieldedTo;
        std::shared_ptr<esp32::Esp32Link> link;
    };

    struct Job {
        bool active = false;
        std::uint64_t generation = 0;
        std::uint64_t holds = 0;
        esp32::Esp32Settings settings;
        std::vector<Hold> blockers;
    };

    struct Scan {
        std::uint64_t generation = 0;
        std::uint64_t holds = 0;
        bool complete = false;
        std::vector<Probe> found;
        std::vector<esp32::SerialPortInfo> ports;
        std::vector<std::string> silent;
        std::vector<std::string> elsewhere;
        std::optional<esp32::OpenFailure> problem;
        std::optional<esp32::BoardTrouble> trouble;
    };

    struct Dropped {
        std::vector<std::shared_ptr<esp32::Esp32Link>> links;
        std::vector<std::string> resources;
    };

    struct Lift {
        std::weak_ptr<esp32::Esp32Link> link;
        Clock::time_point due;
    };

    void run(const std::stop_token& stop);
    void lift(const std::stop_token& stop);
    [[nodiscard]] Scan scanOnce(const Job& job, const std::stop_token& cancel);
    [[nodiscard]] esp32::LinkEvents linkEvents(esp32::LinkMode via) const;
    [[nodiscard]] Job jobLocked() const;
    void requestScanLocked();
    void scheduleLocked(std::chrono::milliseconds delay);
    void wantRefresh();
    void cancelScan();
    void settle();
    void adoptLocked(Scan& scan, Dropped& dropped);
    void refreshLocked(Dropped& dropped);
    void closeAllLocked(Dropped& dropped);
    void syncLocked(Entry& entry) const;
    void dropLinkLocked(Entry& entry, Dropped& dropped) const;
    void finish(Dropped& dropped);
    void notify();
    [[nodiscard]] bool wantsRetryLocked() const;
    [[nodiscard]] Board snapshotLocked(const Entry& entry) const;
    [[nodiscard]] const Hold* holdOverLocked(std::string_view where, esp32::LinkMode via) const;
    [[nodiscard]] bool dismissedLocked(std::string_view where, esp32::LinkMode via) const;
    [[nodiscard]] Entry* entryAtLocked(std::string_view where, esp32::LinkMode via);
    [[nodiscard]] Entry* entryNamedLocked(std::string_view id);
    [[nodiscard]] const Entry* entryNamedLocked(std::string_view id) const;
    void eraseLocked(std::size_t index, Dropped& dropped);

    const FleetHooks m_hooks;
    const Changed m_changed;
    const std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();

    mutable std::mutex m_mutex;
    esp32::Esp32Settings m_settings;
    bool m_active = false;
    bool m_started = false;
    std::uint64_t m_generation = 1;
    std::uint64_t m_holdsVersion = 0;
    std::vector<Entry> m_entries;
    std::map<std::string, Hold, std::less<>> m_holds;
    std::vector<Hold> m_dismissed;
    std::vector<Hold> m_quiet;
    std::map<int, int> m_strikes;
    std::vector<esp32::SerialPortInfo> m_ports;
    std::optional<esp32::OpenFailure> m_problem;
    std::optional<esp32::BoardTrouble> m_trouble;
    std::string m_probing;

    std::mutex m_claimMutex;
    std::map<std::string, int, std::less<>> m_claims;

    mutable std::mutex m_wakeMutex;
    std::condition_variable_any m_wake;
    std::condition_variable_any m_idle;
    bool m_dirty = false;
    bool m_refresh = false;
    bool m_scanning = false;
    std::optional<Clock::time_point> m_due;
    std::stop_source m_cancel;

    std::mutex m_liftMutex;
    std::condition_variable_any m_liftWake;
    std::deque<Lift> m_lifts;

    std::unique_ptr<platform::UsbWatcher> m_watcher;
    std::jthread m_lifter;
    std::jthread m_thread;
};

}
