#include "gs/touchbox/Fleet.h"

#include <gs/support/Log.h>
#include <gs/support/Thread.h>
#include <gs/support/UsbWatcher.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace gs::touchbox {

namespace {

constexpr std::string_view kCategory = "esp32";
constexpr std::string_view kSerialPrefix = "serial:";
constexpr std::chrono::milliseconds kSettleMargin{3000};
constexpr std::chrono::milliseconds kSettleSlice{50};
constexpr std::chrono::milliseconds kWorkerPatience{3000};
constexpr std::chrono::milliseconds kBootHello{1000};
constexpr std::size_t kMostIdLength = 64;
constexpr int kQuietStrikes = 2;
constexpr std::uint16_t kScreenMiddle = 16384;

bool automaticPort(std::string_view port) noexcept
{
    return port.empty() || port == esp32::kAutomaticPort;
}

bool automaticSerial(const esp32::Esp32Settings& settings) noexcept
{
    return settings.link == esp32::LinkMode::Serial && automaticPort(settings.port);
}

esp32::LinkState settleLink(const esp32::Esp32Link& link, std::chrono::milliseconds timeout,
                            const std::stop_token& stop)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    esp32::LinkState state = link.state();
    while (state == esp32::LinkState::Opening && !stop.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        state = link.waitSettled(kSettleSlice);
    }
    return state;
}

esp32::OpenFailure failureOf(esp32::SerialFailure failure) noexcept
{
    switch (failure) {
    case esp32::SerialFailure::Busy:
        return esp32::OpenFailure::PortBusy;
    case esp32::SerialFailure::NotFound:
        return esp32::OpenFailure::PortMissing;
    case esp32::SerialFailure::None:
    case esp32::SerialFailure::Refused:
        break;
    }
    return esp32::OpenFailure::NoAnswer;
}

bool blocks(const Hold& blocker, const Candidate& candidate) noexcept
{
    if (candidate.wifi) {
        return blocker.wifi;
    }
    return covers(blocker, candidate.where, false);
}

bool samePlace(std::string_view where, bool wifi, std::string_view otherWhere, bool otherWifi) noexcept
{
    if (wifi != otherWifi) {
        return false;
    }
    return wifi ? where == otherWhere : samePort(where, otherWhere);
}

bool portListed(const std::vector<esp32::SerialPortInfo>& ports, std::string_view where) noexcept
{
    return std::ranges::any_of(ports, [where](const esp32::SerialPortInfo& port) { return samePort(port.name, where); });
}

char lowered(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

gs_esp32_touch middleTouch(bool down)
{
    gs_esp32_touch touch{};
    touch.count = 1;
    touch.contacts[0] = {.id = 0, .tip = static_cast<std::uint8_t>(down ? 1 : 0), .x = kScreenMiddle, .y = kScreenMiddle};
    return touch;
}

}

std::string boardIdOf(std::string_view where, bool wifi)
{
    std::string id = wifi ? "wifi-" : "serial-";
    for (const char c : where) {
        const char low = lowered(c);
        id.push_back((low >= 'a' && low <= 'z') || (low >= '0' && low <= '9') ? low : '-');
    }
    if (id.size() > kMostIdLength) {
        id.resize(kMostIdLength);
    }
    return id;
}

std::string serialResource(std::string_view where)
{
    const int number = esp32::portNumber(where);
    return std::string(kSerialPrefix) + (number > 0 ? "COM" + std::to_string(number) : std::string(where));
}

bool samePort(std::string_view a, std::string_view b) noexcept
{
    const int number = esp32::portNumber(a);
    return number > 0 && number == esp32::portNumber(b);
}

bool sameResource(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return lowered(x) == lowered(y); });
}

Hold holdOf(const esp32::Esp32Settings& settings)
{
    if (settings.link == esp32::LinkMode::Wifi) {
        return Hold{.where = settings.address, .wifi = true, .every = true};
    }
    const bool automatic = automaticPort(settings.port);
    return Hold{.where = automatic ? std::string() : settings.port, .wifi = false, .every = automatic};
}

bool covers(const Hold& hold, std::string_view where, bool wifi) noexcept
{
    if (hold.wifi != wifi) {
        return false;
    }
    return wifi || hold.every || samePort(hold.where, where);
}

std::vector<Candidate> candidatesOf(const esp32::Esp32Settings& settings,
                                    const std::vector<esp32::SerialPortInfo>& ports, const std::vector<Hold>& blockers)
{
    std::vector<Candidate> candidates;
    const auto add = [&candidates, &blockers](Candidate candidate) {
        if (std::ranges::none_of(blockers, [&candidate](const Hold& blocker) { return blocks(blocker, candidate); })) {
            candidates.push_back(std::move(candidate));
        }
    };
    if (settings.link == esp32::LinkMode::Wifi) {
        add(Candidate{.where = settings.address, .wifi = true, .kind = esp32::PortKind::Other, .chosen = true});
        return candidates;
    }
    if (!automaticPort(settings.port)) {
        if (!esp32::validPortName(settings.port)) {
            return candidates;
        }
        Candidate chosen{.where = settings.port, .wifi = false, .kind = esp32::PortKind::Other, .chosen = true};
        const auto listed = std::ranges::find_if(
            ports, [&settings](const esp32::SerialPortInfo& port) { return samePort(port.name, settings.port); });
        if (listed != ports.end()) {
            chosen.where = listed->name;
            chosen.kind = listed->kind;
        }
        add(std::move(chosen));
        return candidates;
    }
    for (const esp32::SerialPortInfo& port : ports) {
        if (esp32::probedAutomatically(port.kind)) {
            add(Candidate{.where = port.name, .wifi = false, .kind = port.kind, .chosen = false});
        }
    }
    return candidates;
}

Probed probeBoard(const Candidate& candidate, const esp32::Esp32Settings& settings, esp32::LinkEvents events,
                  const std::stop_token& stop, esp32::BoardTrouble& trouble)
{
    trouble = {};
    if (candidate.wifi) {
        esp32::Esp32Settings wifi = settings;
        wifi.link = esp32::LinkMode::Wifi;
        wifi.address = candidate.where;
        Expected<esp32::OpenedLink, esp32::OpenFailure> opened = esp32::openBoard(wifi, std::move(events), stop);
        if (!opened) {
            return makeUnexpected(opened.error());
        }
        return Probe{.link = std::move(opened->link), .where = opened->where, .wifi = true, .kind = esp32::PortKind::Other};
    }
    esp32::SerialFailure failure = esp32::SerialFailure::None;
    std::unique_ptr<esp32::Port> port = esp32::openSerialPort(candidate.where, GS_ESP32_HANDSHAKE_BAUD, failure);
    if (port == nullptr) {
        return makeUnexpected(failureOf(failure));
    }
    esp32::LinkOptions options;
    options.fastBaud = esp32::bridge(candidate.kind);
    options.allowLegacy = candidate.chosen;
    if (!candidate.chosen) {
        options.helloWait = std::max(options.helloWait, kBootHello);
    }
    auto link = std::make_unique<esp32::Esp32Link>(std::move(port), options, std::move(events));
    const esp32::LinkState state = settleLink(*link, options.helloWait + kSettleMargin, stop);
    if (state != esp32::LinkState::Ready && state != esp32::LinkState::Legacy) {
        trouble = link->trouble();
        return makeUnexpected(stop.stop_requested() ? esp32::OpenFailure::Stopped : esp32::OpenFailure::NoAnswer);
    }
    return Probe{.link = std::move(link), .where = candidate.where, .wifi = false, .kind = candidate.kind};
}

bool sameShape(const Board& a, const Board& b) noexcept
{
    return a.id == b.id && a.where == b.where && a.wifi == b.wifi && a.kind == b.kind && a.info == b.info &&
           a.status == b.status && a.trouble == b.trouble && a.legacy == b.legacy && a.connected == b.connected &&
           a.held == b.held && a.inSession == b.inSession && a.flashing == b.flashing && a.yieldedTo == b.yieldedTo;
}

Fleet::Fleet(FleetHooks hooks, Changed changed)
    : m_hooks(std::move(hooks))
    , m_changed(std::move(changed))
{
    m_gate->owner = this;
}

Fleet::~Fleet()
{
    {
        const std::scoped_lock lock(m_gate->mutex);
        m_gate->owner = nullptr;
    }
    m_watcher.reset();
    cancelScan();
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_wake.notify_all();
        m_thread.join();
    }
    if (m_lifter.joinable()) {
        m_lifter.request_stop();
        m_liftWake.notify_all();
        m_lifter.join();
    }
    Dropped dropped;
    {
        const std::scoped_lock lock(m_mutex);
        closeAllLocked(dropped);
    }
    finish(dropped);
    std::map<std::string, int, std::less<>> claims;
    {
        const std::scoped_lock lock(m_claimMutex);
        claims.swap(m_claims);
    }
    if (m_hooks.release) {
        for (const auto& claim : claims) {
            m_hooks.release(claim.first);
        }
    }
}

void Fleet::start()
{
    {
        const std::scoped_lock lock(m_mutex);
        if (m_started) {
            return;
        }
        m_started = true;
    }
    m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
    m_lifter = std::jthread([this](const std::stop_token& stop) { lift(stop); });
    if (m_hooks.watchUsb) {
        const std::shared_ptr<Gate> gate = m_gate;
        m_watcher = std::make_unique<platform::UsbWatcher>([gate] {
            const std::scoped_lock lock(gate->mutex);
            if (gate->owner != nullptr) {
                gate->owner->rescan();
            }
        });
    }
    const std::scoped_lock lock(m_mutex);
    requestScanLocked();
}

void Fleet::setActive(bool active)
{
    Dropped dropped;
    {
        const std::scoped_lock lock(m_mutex);
        if (active == m_active) {
            return;
        }
        m_active = active;
        ++m_generation;
        if (!active) {
            closeAllLocked(dropped);
            m_problem.reset();
            m_trouble.reset();
        }
        cancelScan();
        requestScanLocked();
    }
    finish(dropped);
    log::info(kCategory, "ESP32 touch boxes: {}", active ? "looking for boards" : "not looked for");
    notify();
}

void Fleet::setSettings(esp32::Esp32Settings settings)
{
    Dropped dropped;
    {
        const std::scoped_lock lock(m_mutex);
        if (settings.port == m_settings.port && settings.link == m_settings.link &&
            settings.address == m_settings.address) {
            m_settings.knownBoards = std::move(settings.knownBoards);
            return;
        }
        m_settings = std::move(settings);
        ++m_generation;
        closeAllLocked(dropped);
        m_dismissed.clear();
        m_quiet.clear();
        m_strikes.clear();
        m_problem.reset();
        m_trouble.reset();
        cancelScan();
        requestScanLocked();
    }
    finish(dropped);
    notify();
}

void Fleet::rescan()
{
    const std::scoped_lock lock(m_mutex);
    requestScanLocked();
}

void Fleet::rescanAfter(std::chrono::milliseconds delay)
{
    const std::scoped_lock lock(m_mutex);
    scheduleLocked(delay);
}

bool Fleet::active() const
{
    const std::scoped_lock lock(m_mutex);
    return m_active;
}

bool Fleet::busy() const
{
    const std::scoped_lock wake(m_wakeMutex);
    return m_dirty || m_scanning || m_refresh;
}

esp32::Esp32Settings Fleet::settings() const
{
    const std::scoped_lock lock(m_mutex);
    return m_settings;
}

std::vector<Board> Fleet::boards() const
{
    const std::scoped_lock lock(m_mutex);
    std::vector<Board> found;
    found.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        found.push_back(snapshotLocked(entry));
    }
    return found;
}

std::optional<Board> Fleet::board(std::string_view id) const
{
    const std::scoped_lock lock(m_mutex);
    const Entry* entry = entryNamedLocked(id);
    return entry != nullptr ? std::optional(snapshotLocked(*entry)) : std::nullopt;
}

std::optional<Board> Fleet::matching(const esp32::Esp32Settings& settings) const
{
    const Hold wanted = holdOf(settings);
    const std::scoped_lock lock(m_mutex);
    const Entry* chosen = nullptr;
    for (const Entry& entry : m_entries) {
        if (!covers(wanted, entry.where, entry.wifi)) {
            continue;
        }
        if (chosen == nullptr || (entry.link != nullptr && chosen->link == nullptr)) {
            chosen = &entry;
        }
    }
    return chosen != nullptr ? std::optional(snapshotLocked(*chosen)) : std::nullopt;
}

std::vector<esp32::SerialPortInfo> Fleet::ports() const
{
    const std::scoped_lock lock(m_mutex);
    return m_ports;
}

std::optional<esp32::OpenFailure> Fleet::problem() const
{
    const std::scoped_lock lock(m_mutex);
    return m_problem;
}

std::optional<esp32::BoardTrouble> Fleet::trouble() const
{
    const std::scoped_lock lock(m_mutex);
    return m_trouble;
}

std::shared_ptr<esp32::Esp32Link> Fleet::link(std::string_view id) const
{
    const std::scoped_lock lock(m_mutex);
    const Entry* entry = entryNamedLocked(id);
    return entry != nullptr ? entry->link : nullptr;
}

Taken Fleet::take(std::string_view id, const std::string& owner, Holder holder)
{
    Taken taken;
    {
        const std::scoped_lock lock(m_mutex);
        Entry* entry = entryNamedLocked(id);
        if (entry == nullptr || entry->link == nullptr || holdOverLocked(entry->where, entry->wifi) != nullptr) {
            return taken;
        }
        syncLocked(*entry);
        taken.link = std::move(entry->link);
        if (!entry->wifi) {
            taken.resource = serialResource(entry->where);
        }
        m_holds.insert_or_assign(owner, Hold{.where = entry->where, .wifi = entry->wifi, .every = false, .holder = holder});
        ++m_holdsVersion;
        log::info(kCategory, "ESP32 touch box on {}: handed over to {}", entry->where, owner);
        requestScanLocked();
    }
    notify();
    return taken;
}

void Fleet::hold(const std::string& owner, Hold wanted)
{
    settle();
    Dropped dropped;
    {
        const std::scoped_lock lock(m_mutex);
        for (Entry& entry : m_entries) {
            if (entry.link != nullptr && covers(wanted, entry.where, entry.wifi)) {
                log::info(kCategory, "ESP32 touch box on {}: handed over to {}", entry.where, owner);
                dropLinkLocked(entry, dropped);
            }
        }
        m_holds.insert_or_assign(owner, std::move(wanted));
        ++m_holdsVersion;
        requestScanLocked();
    }
    finish(dropped);
    notify();
}

void Fleet::release(const std::string& owner, std::chrono::milliseconds delay)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (m_holds.erase(owner) == 0) {
            return;
        }
        ++m_holdsVersion;
        if (delay.count() > 0) {
            scheduleLocked(delay);
        } else {
            requestScanLocked();
        }
    }
    notify();
}

bool Fleet::held(std::string_view where, bool wifi) const
{
    const std::scoped_lock lock(m_mutex);
    return holdOverLocked(where, wifi) != nullptr;
}

bool Fleet::inSession(std::string_view where, bool wifi) const
{
    const std::scoped_lock lock(m_mutex);
    return std::ranges::any_of(m_holds, [where, wifi](const std::pair<const std::string, Hold>& hold) {
        return hold.second.holder == Holder::Session && covers(hold.second, where, wifi);
    });
}

bool Fleet::acquire(const std::string& resource, bool wait)
{
    const std::scoped_lock lock(m_claimMutex);
    if (const auto found = m_claims.find(resource); found != m_claims.end()) {
        ++found->second;
        return true;
    }
    if (m_hooks.claim && !m_hooks.claim(resource, wait)) {
        return false;
    }
    m_claims.insert_or_assign(resource, 1);
    return true;
}

void Fleet::unacquire(const std::string& resource)
{
    const std::scoped_lock lock(m_claimMutex);
    const auto found = m_claims.find(resource);
    if (found == m_claims.end()) {
        return;
    }
    if (--found->second > 0) {
        return;
    }
    m_claims.erase(found);
    if (m_hooks.release) {
        m_hooks.release(resource);
    }
}

bool Fleet::yield(std::string_view resource, std::string_view by)
{
    Dropped dropped;
    bool ours = false;
    {
        const std::scoped_lock lock(m_mutex);
        if (!m_probing.empty() && sameResource(m_probing, resource)) {
            return false;
        }
        const std::string_view where =
            resource.size() > kSerialPrefix.size() && sameResource(resource.substr(0, kSerialPrefix.size()), kSerialPrefix)
                ? resource.substr(kSerialPrefix.size())
                : std::string_view();
        if (where.empty() || holdOverLocked(where, false) != nullptr) {
            return where.empty();
        }
        for (Entry& entry : m_entries) {
            if (entry.wifi || !samePort(entry.where, where)) {
                continue;
            }
            ours = ours || entry.link != nullptr;
            dropLinkLocked(entry, dropped);
            entry.yieldedTo = by.empty() ? std::string("?") : std::string(by);
        }
        if (ours) {
            log::info(kCategory, "the serial port {} was let go to {}", where, by);
            scheduleLocked(m_hooks.retry);
        }
    }
    finish(dropped);
    if (ours) {
        notify();
    }
    return true;
}

void Fleet::dismiss(std::string_view id)
{
    Dropped dropped;
    {
        const std::scoped_lock lock(m_mutex);
        const Entry* entry = entryNamedLocked(id);
        if (entry == nullptr) {
            return;
        }
        m_dismissed.push_back(Hold{.where = entry->where, .wifi = entry->wifi});
        log::info(kCategory, "ESP32 touch box on {}: let go until it is plugged in again", entry->where);
        for (std::size_t index = m_entries.size(); index-- > 0;) {
            const Entry& each = m_entries[index];
            if (dismissedLocked(each.where, each.wifi) && holdOverLocked(each.where, each.wifi) == nullptr) {
                eraseLocked(index, dropped);
            }
        }
        requestScanLocked();
    }
    finish(dropped);
    notify();
}

void Fleet::forgive()
{
    {
        const std::scoped_lock lock(m_mutex);
        m_dismissed.clear();
        m_quiet.clear();
        m_strikes.clear();
        requestScanLocked();
    }
    notify();
}

bool Fleet::tap(std::string_view id)
{
    const std::shared_ptr<esp32::Esp32Link> target = link(id);
    if (target == nullptr) {
        return false;
    }
    const esp32::LinkState state = target->state();
    if ((state != esp32::LinkState::Ready && state != esp32::LinkState::Legacy) ||
        !target->sendTouch(middleTouch(true))) {
        return false;
    }
    {
        const std::scoped_lock lock(m_liftMutex);
        m_lifts.push_back(Lift{.link = target, .due = Clock::now() + m_hooks.tapHold});
    }
    m_liftWake.notify_all();
    return true;
}

void Fleet::run(const std::stop_token& stop)
{
    platform::setCurrentThreadName("esp32 boards");
    for (;;) {
        bool scanWanted = false;
        bool refreshWanted = false;
        std::stop_token cancel;
        {
            std::unique_lock wake(m_wakeMutex);
            m_scanning = false;
            m_idle.notify_all();
            const auto due = [this] { return m_due.has_value() && Clock::now() >= *m_due; };
            while (!stop.stop_requested() && !m_dirty && !m_refresh && !due()) {
                if (m_due) {
                    const Clock::time_point until = *m_due;
                    static_cast<void>(m_wake.wait_until(wake, stop, until, [this, &due] { return m_dirty || m_refresh || due(); })); // source-rule-allow R9: a scan asked for later, after the board starts or to look again
                } else {
                    static_cast<void>(
                        m_wake.wait(wake, stop, [this] { return m_dirty || m_refresh || m_due.has_value(); }));
                }
            }
            if (stop.stop_requested()) {
                return;
            }
            if (due()) {
                m_due.reset();
                scanWanted = true;
            }
            scanWanted = scanWanted || m_dirty;
            m_dirty = false;
            refreshWanted = std::exchange(m_refresh, false);
            if (scanWanted) {
                m_scanning = true;
                m_cancel = std::stop_source();
                cancel = m_cancel.get_token();
            }
        }
        if (refreshWanted) {
            Dropped dropped;
            {
                const std::scoped_lock lock(m_mutex);
                refreshLocked(dropped);
            }
            finish(dropped);
            notify();
        }
        if (!scanWanted) {
            continue;
        }
        Job job;
        {
            const std::scoped_lock lock(m_mutex);
            job = jobLocked();
        }
        Scan scan = scanOnce(job, cancel);
        Dropped dropped;
        {
            const std::scoped_lock lock(m_mutex);
            adoptLocked(scan, dropped);
        }
        finish(dropped);
        notify();
    }
}

void Fleet::lift(const std::stop_token& stop)
{
    platform::setCurrentThreadName("esp32 taps");
    std::unique_lock lock(m_liftMutex);
    while (!stop.stop_requested()) {
        if (m_lifts.empty()) {
            static_cast<void>(m_liftWake.wait(lock, stop, [this] { return !m_lifts.empty(); }));
            continue;
        }
        const Clock::time_point due = m_lifts.front().due;
        if (Clock::now() < due) {
            static_cast<void>(m_liftWake.wait_until(lock, stop, due, [] { return false; })); // source-rule-allow R9: a tap keeps the finger down for a moment
            continue;
        }
        const std::weak_ptr<esp32::Esp32Link> target = m_lifts.front().link;
        m_lifts.pop_front();
        lock.unlock();
        if (const std::shared_ptr<esp32::Esp32Link> alive = target.lock()) {
            static_cast<void>(alive->sendTouch(middleTouch(false)));
        }
        lock.lock();
    }
}

Fleet::Scan Fleet::scanOnce(const Job& job, const std::stop_token& cancel)
{
    Scan scan;
    scan.generation = job.generation;
    scan.holds = job.holds;
    if (!job.active) {
        scan.complete = true;
        return scan;
    }
    if (m_hooks.list) {
        scan.ports = m_hooks.list();
    }
    const std::vector<esp32::SerialPortInfo> none;
    const std::vector<esp32::SerialPortInfo>& usable =
        job.settings.link == esp32::LinkMode::Serial ? scan.ports : none;
    const std::vector<Candidate> candidates = candidatesOf(job.settings, usable, job.blockers);
    const bool automatic = automaticSerial(job.settings);
    bool busy = false;
    std::optional<esp32::OpenFailure> last;
    for (const Candidate& candidate : candidates) {
        if (cancel.stop_requested()) {
            return scan;
        }
        const std::string resource = candidate.wifi ? std::string() : serialResource(candidate.where);
        if (!resource.empty() && !acquire(resource, false)) {
            log::info(kCategory, "{} is in use by someone else, so it is not asked", candidate.where);
            busy = true;
            last = esp32::OpenFailure::PortBusy;
            scan.elsewhere.push_back(candidate.where);
            continue;
        }
        {
            const std::scoped_lock lock(m_mutex);
            m_probing = resource;
        }
        esp32::BoardTrouble trouble;
        Probed probed = m_hooks.probe(candidate, job.settings, linkEvents(), cancel, trouble);
        {
            const std::scoped_lock lock(m_mutex);
            m_probing.clear();
        }
        if (probed) {
            log::info(kCategory, "ESP32 touch box found on {}", probed->where);
            scan.found.push_back(std::move(*probed));
            continue;
        }
        if (!resource.empty()) {
            unacquire(resource);
        }
        if (probed.error() == esp32::OpenFailure::Stopped) {
            return scan;
        }
        if (trouble.problem != esp32::BoardProblem::None) {
            if (!scan.trouble || candidate.chosen ||
                (scan.trouble->problem == esp32::BoardProblem::Silent && trouble.problem != esp32::BoardProblem::Silent)) {
                scan.trouble = trouble;
            }
        }
        busy = busy || probed.error() == esp32::OpenFailure::PortBusy;
        last = probed.error();
        if (automatic && probed.error() == esp32::OpenFailure::NoAnswer) {
            scan.silent.push_back(candidate.where);
        }
    }
    if (cancel.stop_requested()) {
        return scan;
    }
    scan.complete = true;
    if (!scan.found.empty()) {
        return scan;
    }
    if (candidatesOf(job.settings, usable, {}).empty()) {
        scan.problem = automaticPort(job.settings.port) ? esp32::OpenFailure::NoBoard : esp32::OpenFailure::PortMissing;
    } else if (!candidates.empty()) {
        scan.problem = automatic ? (busy ? esp32::OpenFailure::PortBusy : esp32::OpenFailure::NoBoard) : last;
    }
    return scan;
}

esp32::LinkEvents Fleet::linkEvents() const
{
    const std::shared_ptr<Gate> gate = m_gate;
    const auto changed = [gate] {
        const std::scoped_lock lock(gate->mutex);
        if (gate->owner != nullptr) {
            gate->owner->wantRefresh();
        }
    };
    esp32::LinkEvents events;
    events.state = [changed](esp32::LinkState) { changed(); };
    events.hello = [changed](const esp32::BoardInfo&, const esp32::BoardStatus&) { changed(); };
    events.status = [changed](const esp32::BoardStatus&) { changed(); };
    return events;
}

Fleet::Job Fleet::jobLocked() const
{
    Job job;
    job.active = m_active;
    job.generation = m_generation;
    job.holds = m_holdsVersion;
    job.settings = m_settings;
    for (const Entry& entry : m_entries) {
        if (entry.link != nullptr || holdOverLocked(entry.where, entry.wifi) != nullptr) {
            job.blockers.push_back(Hold{.where = entry.where, .wifi = entry.wifi});
        }
    }
    for (const auto& held : m_holds) {
        job.blockers.push_back(held.second);
    }
    job.blockers.insert(job.blockers.end(), m_dismissed.begin(), m_dismissed.end());
    if (automaticSerial(m_settings)) {
        job.blockers.insert(job.blockers.end(), m_quiet.begin(), m_quiet.end());
    }
    return job;
}

void Fleet::requestScanLocked()
{
    if (!m_started || !m_active) {
        return;
    }
    {
        const std::scoped_lock wake(m_wakeMutex);
        m_dirty = true;
    }
    m_wake.notify_all();
}

void Fleet::scheduleLocked(std::chrono::milliseconds delay)
{
    if (!m_started || !m_active) {
        return;
    }
    {
        const std::scoped_lock wake(m_wakeMutex);
        const Clock::time_point due = Clock::now() + delay;
        if (!m_due || due < *m_due) {
            m_due = due;
        }
    }
    m_wake.notify_all();
}

void Fleet::wantRefresh()
{
    {
        const std::scoped_lock wake(m_wakeMutex);
        m_refresh = true;
    }
    m_wake.notify_all();
}

void Fleet::cancelScan()
{
    const std::scoped_lock wake(m_wakeMutex);
    m_cancel.request_stop();
}

void Fleet::settle()
{
    std::unique_lock wake(m_wakeMutex);
    m_dirty = false;
    m_cancel.request_stop();
    static_cast<void>(m_idle.wait_for(wake, kWorkerPatience, [this] { return !m_scanning; }));
}

void Fleet::adoptLocked(Scan& scan, Dropped& dropped)
{
    const auto drop = [&dropped](Probe& probe) {
        if (!probe.wifi) {
            dropped.resources.push_back(serialResource(probe.where));
        }
        if (probe.link != nullptr) {
            dropped.links.push_back(std::shared_ptr<esp32::Esp32Link>(std::move(probe.link)));
        }
    };
    if (scan.generation != m_generation || !m_active) {
        for (Probe& probe : scan.found) {
            drop(probe);
        }
        return;
    }
    for (Probe& probe : scan.found) {
        if (holdOverLocked(probe.where, probe.wifi) != nullptr || dismissedLocked(probe.where, probe.wifi)) {
            drop(probe);
            continue;
        }
        Entry* entry = entryAtLocked(probe.where, probe.wifi);
        if (entry == nullptr) {
            m_entries.push_back(Entry{.id = boardIdOf(probe.where, probe.wifi),
                                      .where = probe.where,
                                      .wifi = probe.wifi,
                                      .kind = probe.kind});
            entry = &m_entries.back();
        } else if (entry->link != nullptr) {
            drop(probe);
            continue;
        }
        entry->where = probe.where;
        entry->kind = probe.kind;
        entry->link = std::shared_ptr<esp32::Esp32Link>(std::move(probe.link));
        entry->yieldedTo.clear();
    }
    if (scan.complete && scan.holds == m_holdsVersion) {
        m_problem = scan.problem;
        m_trouble = scan.trouble;
        m_ports = scan.ports;
        for (const Probe& probe : scan.found) {
            m_strikes.erase(esp32::portNumber(probe.where));
        }
        for (const std::string& silent : scan.silent) {
            const bool quiet =
                std::ranges::any_of(m_quiet, [&silent](const Hold& each) { return samePort(each.where, silent); });
            if (!quiet && ++m_strikes[esp32::portNumber(silent)] >= kQuietStrikes) {
                m_quiet.push_back(Hold{.where = silent});
            }
        }
        if (automaticSerial(m_settings)) {
            std::erase_if(m_quiet, [&scan](const Hold& quiet) { return !portListed(scan.ports, quiet.where); });
            std::erase_if(m_strikes, [&scan](const std::pair<const int, int>& strike) {
                return std::ranges::none_of(scan.ports, [&strike](const esp32::SerialPortInfo& port) {
                    return esp32::portNumber(port.name) == strike.first;
                });
            });
        }
        if (m_settings.link == esp32::LinkMode::Serial) {
            std::erase_if(m_dismissed,
                          [&scan](const Hold& place) { return !place.wifi && !portListed(scan.ports, place.where); });
        }
        for (std::size_t index = m_entries.size(); index-- > 0;) {
            const Entry& entry = m_entries[index];
            if (entry.link != nullptr || holdOverLocked(entry.where, entry.wifi) != nullptr) {
                continue;
            }
            const bool kept = !entry.yieldedTo.empty() && !entry.wifi && portListed(scan.ports, entry.where) &&
                              std::ranges::any_of(scan.elsewhere,
                                                  [&entry](const std::string& where) { return samePort(where, entry.where); });
            if (!kept) {
                eraseLocked(index, dropped);
            }
        }
    }
    for (Entry& entry : m_entries) {
        syncLocked(entry);
    }
    if (scan.complete && wantsRetryLocked()) {
        scheduleLocked(m_hooks.retry);
    }
}

void Fleet::refreshLocked(Dropped& dropped)
{
    bool lost = false;
    for (std::size_t index = m_entries.size(); index-- > 0;) {
        Entry& entry = m_entries[index];
        if (entry.link == nullptr) {
            continue;
        }
        const esp32::LinkState state = entry.link->state();
        if (state != esp32::LinkState::Lost && state != esp32::LinkState::Closed) {
            continue;
        }
        log::info(kCategory, "ESP32 touch box on {}: the board stopped answering", entry.where);
        syncLocked(entry);
        dropLinkLocked(entry, dropped);
        lost = true;
        if (holdOverLocked(entry.where, entry.wifi) == nullptr) {
            eraseLocked(index, dropped);
        }
    }
    for (Entry& entry : m_entries) {
        syncLocked(entry);
    }
    if (lost) {
        requestScanLocked();
    }
}

void Fleet::closeAllLocked(Dropped& dropped)
{
    for (std::size_t index = m_entries.size(); index-- > 0;) {
        eraseLocked(index, dropped);
    }
}

void Fleet::syncLocked(Entry& entry) const
{
    if (entry.link == nullptr) {
        return;
    }
    entry.info = entry.link->board();
    entry.status = entry.link->status();
    entry.trouble = entry.link->trouble();
    entry.legacy = entry.link->state() == esp32::LinkState::Legacy;
}

void Fleet::dropLinkLocked(Entry& entry, Dropped& dropped) const
{
    if (entry.link == nullptr) {
        return;
    }
    if (!entry.wifi) {
        dropped.resources.push_back(serialResource(entry.where));
    }
    dropped.links.push_back(std::move(entry.link));
    entry.link.reset();
}

void Fleet::finish(Dropped& dropped)
{
    dropped.links.clear();
    for (const std::string& resource : dropped.resources) {
        unacquire(resource);
    }
    dropped.resources.clear();
}

void Fleet::notify()
{
    if (m_changed) {
        m_changed();
    }
}

bool Fleet::wantsRetryLocked() const
{
    if (!m_started || !m_active) {
        return false;
    }
    const bool wifiMissing = m_settings.link == esp32::LinkMode::Wifi &&
                             std::ranges::none_of(m_entries, [](const Entry& entry) { return entry.wifi; });
    const bool yielded = std::ranges::any_of(m_entries, [](const Entry& entry) { return !entry.yieldedTo.empty(); });
    return wifiMissing || yielded;
}

Board Fleet::snapshotLocked(const Entry& entry) const
{
    Board board;
    board.id = entry.id;
    board.where = entry.where;
    board.wifi = entry.wifi;
    board.kind = entry.kind;
    board.info = entry.info;
    board.status = entry.status;
    board.trouble = entry.trouble;
    board.legacy = entry.legacy;
    board.yieldedTo = entry.yieldedTo;
    board.connected = entry.link != nullptr;
    if (entry.link != nullptr) {
        board.info = entry.link->board();
        board.status = entry.link->status();
        board.roundTrip = entry.link->roundTrip();
        board.legacy = entry.link->state() == esp32::LinkState::Legacy;
        board.trouble = entry.link->trouble();
    }
    if (const Hold* hold = holdOverLocked(entry.where, entry.wifi)) {
        board.held = true;
        board.inSession = hold->holder == Holder::Session;
        board.flashing = hold->holder == Holder::Flash;
    }
    return board;
}

const Hold* Fleet::holdOverLocked(std::string_view where, bool wifi) const
{
    for (const auto& held : m_holds) {
        if (covers(held.second, where, wifi)) {
            return &held.second;
        }
    }
    return nullptr;
}

bool Fleet::dismissedLocked(std::string_view where, bool wifi) const
{
    return std::ranges::any_of(m_dismissed, [where, wifi](const Hold& place) { return covers(place, where, wifi); });
}

Fleet::Entry* Fleet::entryAtLocked(std::string_view where, bool wifi)
{
    const auto found = std::ranges::find_if(
        m_entries, [where, wifi](const Entry& entry) { return samePlace(entry.where, entry.wifi, where, wifi); });
    return found != m_entries.end() ? &*found : nullptr;
}

Fleet::Entry* Fleet::entryNamedLocked(std::string_view id)
{
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found != m_entries.end() ? &*found : nullptr;
}

const Fleet::Entry* Fleet::entryNamedLocked(std::string_view id) const
{
    const auto found = std::ranges::find(m_entries, id, &Entry::id);
    return found != m_entries.end() ? &*found : nullptr;
}

void Fleet::eraseLocked(std::size_t index, Dropped& dropped)
{
    dropLinkLocked(m_entries[index], dropped);
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
}

}
