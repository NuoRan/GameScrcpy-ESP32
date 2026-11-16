#include "Extension.h"

#include "gs/touchbox/Describe.h"

#include <gs/esp32/Boards.h>
#include <gs/esp32/Diagnosis.h>
#include <gs/espflash/Chips.h>
#include <gs/espflash/Flasher.h>
#include <gs/support/Thread.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace gs::touchbox {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kCategory = "esp32";
constexpr std::chrono::milliseconds kBootWait{1500};
constexpr std::chrono::milliseconds kSettleMargin{3000};
constexpr std::chrono::milliseconds kSettleSlice{50};
constexpr std::chrono::milliseconds kKeyAnswer{2000};
constexpr std::chrono::seconds kScanStep{1};
constexpr int kScanSteps = 6;
constexpr int kFindSteps = 10;

std::string textOf(const Json& args, const char* key)
{
    if (!args.is_object()) {
        return {};
    }
    const auto found = args.find(key);
    return found != args.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string compact(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string canonicalPort(std::string_view name)
{
    const int number = esp32::portNumber(name);
    return number > 0 ? "COM" + std::to_string(number) : std::string();
}

double flashFraction(espflash::FlashStage stage, std::size_t done, std::size_t total) noexcept
{
    constexpr double kWritingStart = 0.15;
    constexpr double kWritingEnd = 0.9;
    switch (stage) {
    case espflash::FlashStage::Preparing:
    case espflash::FlashStage::Connecting:
        return 0.02;
    case espflash::FlashStage::Detecting:
        return 0.08;
    case espflash::FlashStage::Erasing:
        return 0.12;
    case espflash::FlashStage::Writing:
        return kWritingStart + (kWritingEnd - kWritingStart) *
                                   (total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0);
    case espflash::FlashStage::Verifying:
        return 0.93;
    case espflash::FlashStage::Restarting:
        return 0.97;
    case espflash::FlashStage::Done:
        return 1.0;
    }
    return 0.0;
}

std::string_view stageName(espflash::FlashStage stage) noexcept
{
    switch (stage) {
    case espflash::FlashStage::Preparing:
        return "preparing";
    case espflash::FlashStage::Connecting:
        return "connecting";
    case espflash::FlashStage::Detecting:
        return "detecting";
    case espflash::FlashStage::Erasing:
        return "erasing";
    case espflash::FlashStage::Writing:
        return "writing";
    case espflash::FlashStage::Verifying:
        return "verifying";
    case espflash::FlashStage::Restarting:
        return "restarting";
    case espflash::FlashStage::Done:
        return "done";
    }
    return "preparing";
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

esp32::LinkState settled(const esp32::Esp32Link& link, std::chrono::milliseconds timeout, const std::stop_token& stop)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    esp32::LinkState state = link.state();
    while (state == esp32::LinkState::Opening && !stop.stop_requested() &&
           std::chrono::steady_clock::now() < deadline) {
        state = link.waitSettled(kSettleSlice);
    }
    return state;
}

struct PortFacts {
    esp32::PortKind kind = esp32::PortKind::Other;
    bool usbSerialJtag = false;
};

PortFacts factsOf(std::string_view port)
{
    PortFacts facts;
    for (const esp32::SerialPortInfo& listed : esp32::listSerialPorts()) {
        if (samePort(listed.name, port)) {
            facts.kind = listed.kind;
            facts.usbSerialJtag = esp32::usbSerialJtag(listed.vendorId, listed.productId);
        }
    }
    return facts;
}

std::string notConnectedText(Language language)
{
    return std::string(pick(language, "The board is not connected right now.", "开发板现在没有连上。"));
}

std::string newFirmwareText(Language language)
{
    return std::string(pick(language, "This needs the new firmware: flash it first.", "这需要新固件：请先刷写固件。"));
}

std::string notSentText(Language language)
{
    return std::string(pick(language, "Could not send this to the board.", "没能把它发给开发板。"));
}

std::string notBoardText(Language language, std::string_view device)
{
    return arg(pick(language, "%1 is neither a board nor a serial port.", "%1 既不是开发板，也不是串口。"), device);
}

std::string notPairableText(Language language, std::string_view device)
{
    return arg(pick(language, "%1 is neither a board on this computer's USB nor a board found over Bluetooth.",
                    "%1 既不是接在这台电脑 USB 上的开发板，也不是经蓝牙找到的开发板。"),
               device);
}

bool radioUsable(esp32::BluetoothState state) noexcept
{
    return state != esp32::BluetoothState::Missing && state != esp32::BluetoothState::Off;
}

void waitScanStep(std::chrono::seconds step, const std::stop_token& stop)
{
    std::mutex mutex;
    std::condition_variable_any never;
    std::unique_lock lock(mutex);
    static_cast<void>(never.wait_for(lock, stop, step, [] { return false; })); // source-rule-allow R9: Bluetooth scanning listens for a set time
}

}

std::int32_t Extension::startJob(JobKind kind, std::uint64_t job, std::string_view args)
{
    std::string copied(args.empty() ? std::string_view("{}") : args);
    if (!Json::accept(copied)) {
        return GS_EXT_BAD_ARGUMENT;
    }
    reapJobs();
    const std::scoped_lock lock(m_jobsMutex);
    if (m_stopping || m_jobs.contains(job)) {
        return GS_EXT_FAILED;
    }
    auto running = std::make_unique<Running>();
    Running& slot = *running;
    slot.thread = std::jthread([this, kind, job, arguments = std::move(copied), &slot](const std::stop_token& stop) {
        runJob(kind, job, arguments, stop);
        slot.done.store(true, std::memory_order_release);
    });
    m_jobs.emplace(job, std::move(running));
    return GS_EXT_OK;
}

void Extension::cancelJob(std::uint64_t job)
{
    const std::scoped_lock lock(m_jobsMutex);
    if (const auto found = m_jobs.find(job); found != m_jobs.end()) {
        found->second->thread.request_stop();
    }
}

void Extension::reapJobs()
{
    std::vector<std::unique_ptr<Running>> finished;
    {
        const std::scoped_lock lock(m_jobsMutex);
        for (auto found = m_jobs.begin(); found != m_jobs.end();) {
            if (found->second->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(found->second));
                found = m_jobs.erase(found);
            } else {
                ++found;
            }
        }
    }
    finished.clear();
}

void Extension::runJob(JobKind kind, std::uint64_t job, const std::string& args, const std::stop_token& stop)
{
    platform::setCurrentThreadName("esp32 job");
    const Json parsed = Json::parse(args, nullptr, false);
    const std::string device = textOf(parsed, "device");
    switch (kind) {
    case JobKind::Flash:
        runFlash(job, device, stop);
        break;
    case JobKind::Wifi:
        runWifi(job, device, textOf(parsed, "ssid"), textOf(parsed, "password"));
        break;
    case JobKind::Rename:
        runRename(job, device, textOf(parsed, "name"));
        break;
    case JobKind::Diagnose:
        runDiagnose(job, device, stop);
        break;
    case JobKind::Scan:
        runScan(job, stop);
        break;
    case JobKind::Pair:
        runPair(job, device, stop);
        break;
    case JobKind::Unpair:
        runUnpair(job, device);
        break;
    }
}

void Extension::runFlash(std::uint64_t job, const std::string& device, const std::stop_token& stop)
{
    const auto failed = [this, job, &device](std::string_view text) {
        log::warning(kCategory, "flashing {} did not work: {}", device, text);
        emit("flash.done", compact(Json{{"device", device}, {"ok", false}, {"text", std::string(text)}}));
        m_host.fail(job, text);
    };
    const std::optional<Board> board = m_fleet->board(device);
    std::string port;
    if (board) {
        if (board->via != esp32::LinkMode::Serial) {
            failed(pick(m_language, "Flashing needs the board's serial port: plug it into this computer.",
                        "刷写要用开发板的串口：请把它接到这台电脑。"));
            return;
        }
        port = board->where;
    } else {
        port = canonicalPort(device);
        if (port.empty()) {
            failed(notBoardText(m_language, device));
            return;
        }
    }
    if (m_fleet->inSession(port, esp32::LinkMode::Serial)) {
        failed(pick(m_language, "A mirror window is using this board. Close it, then flash again.",
                    "投屏窗口正在使用这块开发板，关掉它再刷写。"));
        return;
    }
    if (!m_bundle || m_bundle->images.empty()) {
        failed(flashFailureText(espflash::FlashFailure::NoFirmware, m_language));
        return;
    }
    const std::string owner = "flash:" + std::to_string(job);
    m_host.progress(job, 0.02, flashStageText(espflash::FlashStage::Preparing, m_language));
    bool viaFirmware = false;
    bool answered = false;
    std::string resource;
    if (board && board->connected && !board->held) {
        Taken taken = m_fleet->take(board->id, owner, Holder::Flash);
        if (taken.link != nullptr && taken.link->state() == esp32::LinkState::Ready) {
            const std::optional<esp32::BoardInfo> info = taken.link->board();
            answered = info.has_value();
            viaFirmware = info && info->chipId != GS_ESP32_CHIP_ESP32 && taken.link->enterDownload();
        }
        taken.link.reset();
        resource = std::move(taken.resource);
    }
    m_fleet->hold(owner, Hold{.where = port, .via = esp32::LinkMode::Serial, .every = false, .holder = Holder::Flash});
    if (resource.empty()) {
        resource = serialResource(port);
        if (!m_fleet->acquire(resource, true)) {
            m_fleet->release(owner);
            failed(flashFailureText(espflash::FlashFailure::PortBusy, m_language));
            return;
        }
    }
    const PortFacts facts = factsOf(port);
    espflash::FlashRequest request;
    request.port = port;
    request.kind = facts.kind;
    request.usbSerialJtag = facts.usbSerialJtag;
    request.enter = viaFirmware ? espflash::EnterMode::Firmware : espflash::EnterMode::Auto;
    request.firmwareFolder = m_firmwareFolder;
    request.keepSettings = answered;
    espflash::FlashHooks hooks;
    hooks.stop = stop;
    double reached = 0.0;
    int shownPercent = -1;
    std::optional<espflash::FlashStage> shownStage;
    const espflash::FlashProgress progress = [&](espflash::FlashStage stage, std::size_t done, std::size_t total) {
        reached = std::max(reached, flashFraction(stage, done, total));
        const int percent = static_cast<int>(std::lround(reached * 100.0));
        if (percent == shownPercent && shownStage == stage) {
            return;
        }
        shownPercent = percent;
        shownStage = stage;
        const std::string text = flashStageText(stage, m_language);
        m_host.progress(job, reached, text);
        emit("flash.progress", compact(Json{{"device", device},
                                            {"port", port},
                                            {"stage", std::string(stageName(stage))},
                                            {"fraction", reached},
                                            {"text", text}}));
    };
    const Expected<espflash::FlashResult, espflash::FlashFailure> result =
        espflash::flashFirmware(request, progress, hooks);
    m_fleet->release(owner, kBootWait); // source-rule-allow R9: the board needs a moment to start after flashing
    m_fleet->unacquire(resource);
    if (!result) {
        failed(flashFailureText(result.error(), m_language));
        return;
    }
    const std::string chip(espflash::chipInfo(result->chip).name);
    std::string text = arg(pick(m_language, "Firmware %1 is on the %2. The board restarted.",
                                "%2 已写入固件 %1，开发板已重启。"),
                           result->version, chip);
    if (answered && !result->keptSettings) {
        text += pick(m_language,
                     " Its earlier settings could not be kept; set up Wi-Fi and Bluetooth again if you used them.",
                     "开发板原来的设置没能保留；用过 Wi-Fi 或蓝牙的话，请重新设置。");
    }
    log::info(kCategory, "wrote firmware {} to the {} on {}{}", result->version, chip, port,
              result->keptSettings ? ", keeping its settings" : "");
    emit("flash.done", compact(Json{{"device", device},
                                    {"port", port},
                                    {"ok", true},
                                    {"text", text},
                                    {"version", result->version},
                                    {"chip", chip},
                                    {"keptSettings", result->keptSettings}}));
    m_host.finish(job, compact(Json{{"port", port},
                                    {"chip", chip},
                                    {"version", result->version},
                                    {"bytes", result->bytes},
                                    {"compressed", result->compressed},
                                    {"keptSettings", result->keptSettings},
                                    {"text", text}}));
}

void Extension::runWifi(std::uint64_t job, const std::string& device, const std::string& ssid,
                        const std::string& password)
{
    const std::optional<Board> board = m_fleet->board(device);
    if (!board || !board->connected) {
        m_host.fail(job, notConnectedText(m_language));
        return;
    }
    if (board->via != esp32::LinkMode::Serial) {
        m_host.fail(job, pick(m_language, "Wi-Fi is set over the serial port: connect the board with its cable first.",
                              "Wi-Fi 要经串口设置：请先用数据线连上开发板。"));
        return;
    }
    const std::shared_ptr<esp32::Esp32Link> link = m_fleet->link(device);
    const std::optional<esp32::BoardInfo> info = link != nullptr ? link->board() : std::nullopt;
    if (link == nullptr || link->state() != esp32::LinkState::Ready || !info) {
        m_host.fail(job, newFirmwareText(m_language));
        return;
    }
    if ((info->features & GS_ESP32_FEATURE_WIFI) == 0) {
        m_host.fail(job, pick(m_language, "This board has no Wi-Fi.", "这块开发板没有 Wi-Fi。"));
        return;
    }
    const std::string id = esp32::boardId(info->mac);
    const std::optional<esp32::KnownBoard> book = esp32::findBoard(knownBoards(), id);
    if (ssid.empty()) {
        m_host.progress(job, -1.0, pick(m_language, "Forgetting the Wi-Fi settings…", "正在删除 Wi-Fi 设置…"));
        if (!link->forgetWifi()) {
            m_host.fail(job, notSentText(m_language));
            return;
        }
        if (book && (info->features & GS_ESP32_FEATURE_BLE_LINK) != 0) {
            esp32::KnownBoard kept = *book;
            kept.address.clear();
            rememberBoard(kept);
        } else {
            forgetBoard(id);
        }
        log::info(kCategory, "the board {} forgot its Wi-Fi", id);
        m_host.finish(job, compact(Json{{"board", id}, {"forgotten", true}}));
        return;
    }
    if (ssid.size() > static_cast<std::size_t>(GS_ESP32_MAX_SSID)) {
        m_host.fail(job, pick(m_language, "The network name is longer than 32 bytes.", "网络名称超过 32 字节。"));
        return;
    }
    if (password.size() > static_cast<std::size_t>(GS_ESP32_MAX_PASSWORD)) {
        m_host.fail(job, pick(m_language, "The password is longer than 64 bytes.", "密码超过 64 字节。"));
        return;
    }
    m_host.progress(job, -1.0,
                    pick(m_language, "Sending the Wi-Fi settings to the board…", "正在把 Wi-Fi 设置发给开发板…"));
    const esp32::BoardKey key = book ? book->key : esp32::newBoardKey();
    if (!link->setWifi(ssid, password, key)) {
        m_host.fail(job, notSentText(m_language));
        return;
    }
    rememberBoard(esp32::KnownBoard{
        .id = id, .name = info->name, .key = key, .address = {}, .bluetooth = book ? book->bluetooth : std::string()});
    log::info(kCategory, "Wi-Fi settings went to the board {}", id);
    m_host.finish(job, compact(Json{{"board", id}, {"configured", true}}));
}

void Extension::runRename(std::uint64_t job, const std::string& device, const std::string& name)
{
    const std::shared_ptr<esp32::Esp32Link> link = m_fleet->link(device);
    const std::optional<esp32::BoardInfo> info = link != nullptr ? link->board() : std::nullopt;
    if (link == nullptr) {
        m_host.fail(job, notConnectedText(m_language));
        return;
    }
    if (link->state() != esp32::LinkState::Ready || !info) {
        m_host.fail(job, newFirmwareText(m_language));
        return;
    }
    if (name.empty() || name.size() > static_cast<std::size_t>(GS_ESP32_MAX_NAME)) {
        m_host.fail(job, pick(m_language, "The name has to be 1 to 32 bytes.", "名称要有 1～32 个字节。"));
        return;
    }
    m_host.progress(job, -1.0, pick(m_language, "Renaming the board…", "正在重命名开发板…"));
    if (!link->setName(name)) {
        m_host.fail(job, notSentText(m_language));
        return;
    }
    const std::string id = esp32::boardId(info->mac);
    std::optional<std::vector<esp32::KnownBoard>> known;
    {
        const std::scoped_lock lock(m_mutex);
        const auto found = std::ranges::find(m_known, id, &esp32::KnownBoard::id);
        if (found != m_known.end() && found->name != name) {
            found->name = name;
            known = m_known;
        }
    }
    if (known) {
        saveKnown(*known);
    }
    log::info(kCategory, "the board {} is now called {}", id, name);
    m_host.finish(job, compact(Json{{"board", id}, {"name", name}}));
}

void Extension::runDiagnose(std::uint64_t job, const std::string& device, const std::stop_token& stop)
{
    const auto report = [this, job, &device](bool ok, const Json& found, const std::string& problem) {
        Json event = found.is_object() ? found : Json::object();
        event["device"] = device;
        event["ok"] = ok;
        event["problem"] = problem;
        emit("board.diagnosed", compact(event));
        if (ok) {
            m_host.finish(job, compact(event));
        } else {
            m_host.fail(job, problem);
        }
    };
    const std::optional<Board> board = m_fleet->board(device);
    if (board && board->connected) {
        if (const std::shared_ptr<esp32::Esp32Link> link = m_fleet->link(device)) {
            Json found = Json::parse(boardJson(*board, m_bundle, m_language), nullptr, false);
            if (found.is_object()) {
                found["baud"] = link->datagrams() ? Json(nullptr) : Json(link->baud());
            }
            report(true, found, board->legacy ? troubleText(board->trouble, m_language) : std::string());
            return;
        }
    }
    if (board && (board->held || !board->yieldedTo.empty())) {
        report(false, Json::object(), boardStateText(*board, m_language));
        return;
    }
    if (board && board->via != esp32::LinkMode::Serial) {
        report(false, Json::object(), openFailureText(esp32::OpenFailure::NoBoard, board->via, m_language));
        return;
    }
    const std::string port = board ? board->where : canonicalPort(device);
    if (port.empty()) {
        report(false, Json::object(), notBoardText(m_language, device));
        return;
    }
    const std::string owner = "diagnose:" + std::to_string(job);
    m_fleet->hold(owner, Hold{.where = port, .via = esp32::LinkMode::Serial, .every = false, .holder = Holder::Job});
    const std::string resource = serialResource(port);
    if (!m_fleet->acquire(resource, true)) {
        m_fleet->release(owner);
        report(false, Json::object(), flashFailureText(espflash::FlashFailure::PortBusy, m_language));
        return;
    }
    m_host.progress(job, -1.0, arg(pick(m_language, "Opening %1…", "正在打开 %1…"), port));
    const PortFacts facts = factsOf(port);
    esp32::SerialFailure failure = esp32::SerialFailure::None;
    std::unique_ptr<esp32::Port> opened = esp32::openSerialPort(port, GS_ESP32_HANDSHAKE_BAUD, failure);
    Json found = Json::object();
    bool ok = false;
    std::string problem;
    if (opened == nullptr) {
        problem = openFailureText(failureOf(failure), esp32::LinkMode::Serial, m_language);
    } else {
        m_host.progress(job, -1.0, pick(m_language, "Asking the board…", "正在询问开发板…"));
        esp32::LinkOptions options;
        options.fastBaud = esp32::bridge(facts.kind);
        options.allowLegacy = true;
        esp32::Esp32Link link(std::move(opened), options);
        const esp32::LinkState state = settled(link, options.helloWait + kSettleMargin, stop);
        if (state == esp32::LinkState::Ready || state == esp32::LinkState::Legacy) {
            Board seen;
            seen.id = boardIdOf(port, esp32::LinkMode::Serial);
            seen.where = port;
            seen.kind = facts.kind;
            seen.info = link.board();
            seen.status = link.status();
            seen.roundTrip = link.roundTrip();
            seen.trouble = link.trouble();
            seen.legacy = state == esp32::LinkState::Legacy;
            seen.connected = true;
            found = Json::parse(boardJson(seen, m_bundle, m_language), nullptr, false);
            if (found.is_object()) {
                found["baud"] = link.baud();
            }
            ok = true;
            problem = seen.legacy ? troubleText(seen.trouble, m_language) : std::string();
        } else if (stop.stop_requested()) {
            problem = pick(m_language, "The check was cancelled.", "已取消检查。");
        } else {
            problem = troubleText(link.trouble(), m_language);
            if (problem.empty()) {
                problem = openFailureText(esp32::OpenFailure::NoAnswer, esp32::LinkMode::Serial, m_language);
            }
        }
    }
    m_fleet->release(owner);
    m_fleet->unacquire(resource);
    report(ok, found, problem);
}

bool Extension::usable(std::uint64_t job) const
{
    const esp32::BluetoothState state = m_radio->state();
    if (radioUsable(state)) {
        return true;
    }
    m_host.fail(job, bluetoothStateText(state, m_language));
    return false;
}

std::optional<esp32::SeenBoard> Extension::lookFor(std::string_view id, std::uint64_t job, double from,
                                                   const std::stop_token& stop)
{
    const auto seenNow = [this, id]() -> std::optional<esp32::SeenBoard> {
        for (const esp32::SeenBoard& each : m_radio->seen(esp32::kSeenRecently)) {
            if (each.advert.id == id) {
                return each;
            }
        }
        return std::nullopt;
    };
    std::optional<esp32::SeenBoard> found = seenNow();
    if (found) {
        return found;
    }
    m_radio->watch(true);
    for (int step = 0; step < kFindSteps && !stop.stop_requested() && !found; ++step) {
        m_host.progress(job, from + (0.5 - from) * static_cast<double>(step) / kFindSteps,
                        pick(m_language, "Looking for the board over Bluetooth…", "正在经蓝牙寻找开发板…"));
        waitScanStep(kScanStep, stop);
        found = seenNow();
    }
    m_radio->watch(false);
    return found;
}

void Extension::runScan(std::uint64_t job, const std::stop_token& stop)
{
    if (!usable(job)) {
        return;
    }
    m_scans.fetch_add(1, std::memory_order_acq_rel);
    m_radio->watch(true);
    for (int step = 0; step < kScanSteps && !stop.stop_requested(); ++step) {
        const std::size_t count = m_radio->seen(esp32::kSeenRecently).size();
        m_host.progress(job, static_cast<double>(step) / kScanSteps,
                        arg(pick(m_language, "Looking for boards over Bluetooth… %1 found", "正在经蓝牙查找开发板…已找到 %1 块"),
                            std::to_string(count)));
        emit("bluetooth.changed", bluetoothJson());
        waitScanStep(kScanStep, stop);
    }
    m_radio->watch(false);
    m_scans.fetch_sub(1, std::memory_order_acq_rel);
    const std::string listed = bluetoothJson();
    emit("bluetooth.changed", listed);
    if (stop.stop_requested()) {
        m_host.fail(job, pick(m_language, "The search was cancelled.", "已取消查找。"));
        return;
    }
    m_host.finish(job, listed);
}

void Extension::runPair(std::uint64_t job, const std::string& device, const std::stop_token& stop)
{
    const auto failed = [this, job](std::string_view text) {
        log::warning(kCategory, "pairing over Bluetooth did not work: {}", text);
        m_host.fail(job, text);
    };
    const auto unseen = [this, &stop]() -> std::string {
        return std::string(stop.stop_requested()
                               ? pick(m_language, "Pairing was cancelled.", "已取消配对。")
                               : pick(m_language,
                                      "Windows does not see the board over Bluetooth. Keep it on and near this "
                                      "computer, then pair again.",
                                      "Windows 经蓝牙看不到这块开发板。让它保持通电、靠近这台电脑，再配对一次。"));
    };
    if (!usable(job)) {
        return;
    }
    const std::vector<esp32::KnownBoard> known = knownBoards();
    const std::optional<esp32::KnownBoard> named = esp32::findBoard(known, device);
    std::optional<std::uint64_t> address = named ? std::nullopt : esp32::parseBluetooth(device);
    std::string id;
    std::string name;
    esp32::BoardKey key{};
    if (named) {
        id = named->id;
        name = named->name;
        key = named->key;
        address = esp32::parseBluetooth(named->bluetooth);
        if (!address) {
            const std::optional<esp32::SeenBoard> spotted = lookFor(id, job, 0.1, stop);
            if (!spotted) {
                failed(unseen());
                return;
            }
            if (!spotted->advert.keyed) {
                failed(openFailureText(esp32::OpenFailure::NoKey, esp32::LinkMode::Bluetooth, m_language));
                return;
            }
            address = spotted->address;
            if (name.empty()) {
                name = spotted->name;
            }
        }
    } else if (!address) {
        const std::optional<Board> board = m_fleet->board(device);
        if (!board) {
            failed(notPairableText(m_language, device));
            return;
        }
        if (board->via != esp32::LinkMode::Serial) {
            failed(pick(m_language,
                        "Pair a board while it is plugged into this computer by USB, or pick it from the boards found "
                        "over Bluetooth.",
                        "请在开发板经 USB 接在这台电脑上时配对，或者从经蓝牙找到的开发板里选它。"));
            return;
        }
        const std::shared_ptr<esp32::Esp32Link> link = m_fleet->link(device);
        const std::optional<esp32::BoardInfo> info = link != nullptr ? link->board() : std::nullopt;
        if (link == nullptr || link->state() != esp32::LinkState::Ready || !info) {
            failed(link == nullptr ? notConnectedText(m_language) : newFirmwareText(m_language));
            return;
        }
        if ((info->features & GS_ESP32_FEATURE_BLE_LINK) == 0) {
            failed(pick(m_language,
                        "This board's firmware cannot talk to a computer over Bluetooth. Flash firmware 1.1 or later; "
                        "Bluetooth works on the classic ESP32, C3, C6 and S3.",
                        "这块开发板的固件还不能经蓝牙连电脑。请刷入 1.1 或更新的固件；经典 ESP32、C3、C6、S3 才有蓝牙。"));
            return;
        }
        id = esp32::boardId(info->mac);
        name = info->name;
        const std::optional<esp32::KnownBoard> book = esp32::findBoard(known, id);
        key = book ? book->key : esp32::newBoardKey();
        m_host.progress(job, 0.05, pick(m_language, "Giving the board its key…", "正在把密钥交给开发板…"));
        const std::uint32_t before = link->hellos();
        if (!link->setKey(key)) {
            failed(notSentText(m_language));
            return;
        }
        const std::optional<esp32::BoardInfo> answered =
            link->waitHello(before, kKeyAnswer) ? link->board() : std::nullopt;
        if (!answered || (answered->features & GS_ESP32_FEATURE_KEYED) == 0) {
            failed(pick(m_language, "The board did not take the key. Flash the firmware again, then pair again.",
                        "开发板没有收下密钥。请重新刷写固件后再配对。"));
            return;
        }
        rememberBoard(esp32::KnownBoard{.id = id,
                                        .name = name,
                                        .key = key,
                                        .address = book ? book->address : std::string(),
                                        .bluetooth = book ? book->bluetooth : std::string()});
        if (book) {
            address = esp32::parseBluetooth(book->bluetooth);
        }
        if (!address) {
            const std::optional<esp32::SeenBoard> spotted = lookFor(id, job, 0.1, stop);
            if (!spotted) {
                failed(unseen());
                return;
            }
            address = spotted->address;
        }
    } else {
        const auto spottedAt = [this](std::uint64_t at) -> std::optional<esp32::SeenBoard> {
            for (const esp32::SeenBoard& each : m_radio->seen(esp32::kSeenRecently)) {
                if (each.address == at) {
                    return each;
                }
            }
            return std::nullopt;
        };
        std::optional<esp32::SeenBoard> spotted = spottedAt(*address);
        if (!spotted && m_radio->find(*address, std::chrono::milliseconds{kFindSteps * 1000}, stop)) {
            spotted = spottedAt(*address);
        }
        if (!spotted) {
            failed(pick(m_language, "This Bluetooth address is not an ESP32 touch box nearby. Look for boards again.",
                        "这个蓝牙地址不是附近的 ESP32 触控盒。请重新查找。"));
            return;
        }
        id = spotted->advert.id;
        const std::optional<esp32::KnownBoard> book = esp32::findBoard(known, id);
        if (!book || !spotted->advert.keyed) {
            failed(openFailureText(esp32::OpenFailure::NoKey, esp32::LinkMode::Bluetooth, m_language));
            return;
        }
        key = book->key;
        name = book->name.empty() ? spotted->name : book->name;
    }
    m_host.progress(job, 0.55, pick(m_language, "Pairing with Windows…", "正在与 Windows 配对…"));
    const bool pairedBefore = m_radio->paired(*address);
    const esp32::PairOutcome paired = m_radio->pair(*address);
    if (paired != esp32::PairOutcome::Paired) {
        failed(pairFailureText(paired, m_language));
        return;
    }
    const std::string addressText = esp32::bluetoothText(*address);
    const std::optional<esp32::KnownBoard> book = esp32::findBoard(knownBoards(), id);
    esp32::KnownBoard remembered = book ? *book : esp32::KnownBoard{.id = id, .name = name, .key = key};
    remembered.bluetooth = addressText;
    rememberBoard(remembered);
    m_host.progress(job, 0.75, pick(m_language, "Checking the Bluetooth link…", "正在检查蓝牙连接…"));
    const std::string owner = "pair:" + std::to_string(job);
    m_fleet->hold(owner,
                  Hold{.where = addressText, .via = esp32::LinkMode::Bluetooth, .every = false, .holder = Holder::Job});
    esp32::Esp32Settings settings = globalSettings();
    settings.link = esp32::LinkMode::Bluetooth;
    settings.address = addressText;
    Expected<esp32::OpenedLink, esp32::OpenFailure> opened = esp32::openBoard(settings, {}, stop);
    if (!opened && pairedBefore && !stop.stop_requested() &&
        (opened.error() == esp32::OpenFailure::NotPaired || opened.error() == esp32::OpenFailure::NoBoard ||
         opened.error() == esp32::OpenFailure::NoAnswer)) {
        log::info(kCategory, "the earlier Windows pairing with {} does not work; pairing again", addressText);
        m_host.progress(job, 0.85, pick(m_language, "Pairing with Windows again…", "正在重新与 Windows 配对…"));
        if (m_radio->unpair(*address) && m_radio->pair(*address) == esp32::PairOutcome::Paired) {
            opened = esp32::openBoard(settings, {}, stop);
        }
    }
    if (opened) {
        opened->link.reset();
    }
    m_fleet->release(owner);
    emit("bluetooth.changed", bluetoothJson());
    if (!opened) {
        failed(openFailureText(opened.error(), esp32::LinkMode::Bluetooth, m_language));
        return;
    }
    log::info(kCategory, "the board {} is paired over Bluetooth as {}", id, addressText);
    m_host.finish(job, compact(Json{{"board", id}, {"address", addressText}, {"name", name}, {"paired", true}}));
}

void Extension::runUnpair(std::uint64_t job, const std::string& device)
{
    std::optional<std::uint64_t> address;
    const std::vector<esp32::KnownBoard> known = knownBoards();
    const auto touchBoxAt = [this, &known](std::uint64_t at) {
        const std::string text = esp32::bluetoothText(at);
        return esp32::findBluetooth(known, text).has_value() ||
               std::ranges::any_of(m_radio->seen(esp32::kSeenRecently),
                                   [at](const esp32::SeenBoard& each) { return each.address == at; }) ||
               std::ranges::any_of(m_fleet->boards(), [&text](const Board& each) {
                   return each.via == esp32::LinkMode::Bluetooth && esp32::sameBluetooth(each.where, text);
               });
    };
    if (const std::optional<esp32::KnownBoard> book = esp32::findBoard(known, device)) {
        address = esp32::parseBluetooth(book->bluetooth);
    } else if (const std::optional<Board> board = m_fleet->board(device);
               board && board->via == esp32::LinkMode::Bluetooth) {
        address = esp32::parseBluetooth(board->where);
    } else if (const std::optional<std::uint64_t> typed = esp32::parseBluetooth(device); typed && touchBoxAt(*typed)) {
        address = typed;
    }
    if (!address) {
        m_host.fail(job, notPairableText(m_language, device));
        return;
    }
    const std::string addressText = esp32::bluetoothText(*address);
    const std::string owner = "unpair:" + std::to_string(job);
    m_fleet->hold(owner,
                  Hold{.where = addressText, .via = esp32::LinkMode::Bluetooth, .every = false, .holder = Holder::Job});
    const bool unpaired = m_radio->unpair(*address);
    if (std::optional<esp32::KnownBoard> book = esp32::findBluetooth(known, addressText)) {
        book->bluetooth.clear();
        rememberBoard(*book);
    }
    m_fleet->release(owner);
    emit("bluetooth.changed", bluetoothJson());
    log::info(kCategory, "the Bluetooth pairing with {} was forgotten", addressText);
    m_host.finish(job, compact(Json{{"address", addressText}, {"unpaired", unpaired}}));
}

}
