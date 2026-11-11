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
#include <utility>

namespace gs::touchbox {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kCategory = "esp32";
constexpr std::chrono::milliseconds kBootWait{1500};
constexpr std::chrono::milliseconds kSettleMargin{3000};
constexpr std::chrono::milliseconds kSettleSlice{50};

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
        if (board->wifi) {
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
    if (m_fleet->inSession(port, false)) {
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
    std::string resource;
    if (board && board->connected && !board->held) {
        Taken taken = m_fleet->take(board->id, owner, Holder::Flash);
        if (taken.link != nullptr && taken.link->state() == esp32::LinkState::Ready) {
            const std::optional<esp32::BoardInfo> info = taken.link->board();
            viaFirmware = info && info->chipId != GS_ESP32_CHIP_ESP32 && taken.link->enterDownload();
        }
        taken.link.reset();
        resource = std::move(taken.resource);
    }
    m_fleet->hold(owner, Hold{.where = port, .wifi = false, .every = false, .holder = Holder::Flash});
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
    const std::string text = arg(pick(m_language, "Firmware %1 is on the %2. The board restarted.",
                                      "%2 已写入固件 %1，开发板已重启。"),
                                 result->version, chip);
    log::info(kCategory, "wrote firmware {} to the {} on {}", result->version, chip, port);
    emit("flash.done", compact(Json{{"device", device},
                                    {"port", port},
                                    {"ok", true},
                                    {"text", text},
                                    {"version", result->version},
                                    {"chip", chip}}));
    m_host.finish(job, compact(Json{{"port", port},
                                    {"chip", chip},
                                    {"version", result->version},
                                    {"bytes", result->bytes},
                                    {"compressed", result->compressed},
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
    if (board->wifi) {
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
    if (ssid.empty()) {
        m_host.progress(job, -1.0, pick(m_language, "Forgetting the Wi-Fi settings…", "正在删除 Wi-Fi 设置…"));
        if (!link->forgetWifi()) {
            m_host.fail(job, notSentText(m_language));
            return;
        }
        forgetBoard(id);
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
    const esp32::BoardKey key = esp32::newBoardKey();
    if (!link->setWifi(ssid, password, key)) {
        m_host.fail(job, notSentText(m_language));
        return;
    }
    rememberBoard(esp32::KnownBoard{.id = id, .name = info->name, .key = key, .address = {}});
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
    if (board && board->wifi) {
        report(false, Json::object(), openFailureText(esp32::OpenFailure::NoBoard, true, m_language));
        return;
    }
    const std::string port = board ? board->where : canonicalPort(device);
    if (port.empty()) {
        report(false, Json::object(), notBoardText(m_language, device));
        return;
    }
    const std::string owner = "diagnose:" + std::to_string(job);
    m_fleet->hold(owner, Hold{.where = port, .wifi = false, .every = false, .holder = Holder::Job});
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
        problem = openFailureText(failureOf(failure), false, m_language);
    } else {
        m_host.progress(job, -1.0, pick(m_language, "Asking the board…", "正在询问开发板…"));
        esp32::LinkOptions options;
        options.fastBaud = esp32::bridge(facts.kind);
        options.allowLegacy = true;
        esp32::Esp32Link link(std::move(opened), options);
        const esp32::LinkState state = settled(link, options.helloWait + kSettleMargin, stop);
        if (state == esp32::LinkState::Ready || state == esp32::LinkState::Legacy) {
            Board seen;
            seen.id = boardIdOf(port, false);
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
                problem = openFailureText(esp32::OpenFailure::NoAnswer, false, m_language);
            }
        }
    }
    m_fleet->release(owner);
    m_fleet->unacquire(resource);
    report(ok, found, problem);
}

}
