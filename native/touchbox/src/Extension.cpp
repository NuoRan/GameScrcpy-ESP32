#include "Extension.h"

#include "gs/touchbox/Describe.h"

#include <gs/support/Files.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace gs::touchbox {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kCategory = "esp32";
constexpr std::string_view kSessionPrefix = "session:";

std::string textIn(const Json& object, const char* key)
{
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string idJson(std::string_view id)
{
    return Json{{"id", std::string(id)}}.dump();
}

esp32::ContactAction actionOf(std::uint32_t action) noexcept
{
    switch (action) {
    case GS_EXT_TOUCH_DOWN:
        return esp32::ContactAction::Down;
    case GS_EXT_TOUCH_UP:
        return esp32::ContactAction::Up;
    case GS_EXT_TOUCH_CANCEL:
        return esp32::ContactAction::Cancel;
    default:
        return esp32::ContactAction::Move;
    }
}

std::uint16_t pictureUnits(double ratio) noexcept
{
    return static_cast<std::uint16_t>(std::lround(std::clamp(ratio, 0.0, 1.0) * 65535.0));
}

std::string_view linkStateName(std::optional<esp32::LinkState> state) noexcept
{
    if (!state) {
        return "closed";
    }
    switch (*state) {
    case esp32::LinkState::Opening:
        return "still opening";
    case esp32::LinkState::Ready:
        return "ready";
    case esp32::LinkState::Legacy:
        return "on the old protocol";
    case esp32::LinkState::Lost:
        return "lost";
    case esp32::LinkState::Closed:
        return "closed";
    }
    return "unknown";
}

std::string_view openFailureName(esp32::OpenFailure failure) noexcept
{
    switch (failure) {
    case esp32::OpenFailure::NoBoard:
        return "no ESP32 touch box was found";
    case esp32::OpenFailure::PortMissing:
        return "the chosen serial port is not there";
    case esp32::OpenFailure::PortBusy:
        return "another program is using the serial port";
    case esp32::OpenFailure::NoAnswer:
        return "the board does not answer; flash its firmware";
    case esp32::OpenFailure::NoKey:
        return "Wi-Fi has not been set up over the serial port yet";
    case esp32::OpenFailure::BadAddress:
        return "the board's address is not valid";
    case esp32::OpenFailure::Stopped:
        return "opening was stopped";
    case esp32::OpenFailure::NoBluetooth:
        return "this computer's Bluetooth is missing or off";
    case esp32::OpenFailure::NotPaired:
        return "no board is paired over Bluetooth";
    }
    return "the board could not be opened";
}

std::string_view bluetoothStateName(esp32::BluetoothState state) noexcept
{
    switch (state) {
    case esp32::BluetoothState::Missing:
        return "missing";
    case esp32::BluetoothState::Off:
        return "off";
    case esp32::BluetoothState::Ready:
        return "ready";
    case esp32::BluetoothState::Unknown:
        break;
    }
    return "unknown";
}

}

void Extension::Sink::write(const log::Record& record) noexcept
{
    const std::uint32_t level = record.level == log::Level::Error     ? GS_EXT_LOG_ERROR
                                : record.level == log::Level::Warning ? GS_EXT_LOG_WARNING
                                                                      : GS_EXT_LOG_INFO;
    m_host.log(level, record.message);
}

Extension::Extension(const gs_ext_host* host, PluginOptions options)
    : m_host(host)
    , m_language(systemLanguage())
    , m_sink(m_host)
{
    log::setSink(&m_sink);
    const std::filesystem::path folder = options.folder.empty() ? platform::moduleFolder() : options.folder;
    m_firmwareFolder = folder / std::string(espflash::kFirmwareFolder);
    if (Expected<espflash::FirmwareBundle, espflash::FirmwareProblem> bundle =
            espflash::loadFirmwareBundle(m_firmwareFolder)) {
        m_bundle = std::move(*bundle);
    } else {
        log::warning(kCategory, "no firmware could be read from the extension's firmware folder");
    }
    m_known = loadKnown();
    m_radio = options.radio != nullptr ? options.radio : esp32::systemBluetooth();
    static_cast<void>(m_radio->state());
    FleetHooks hooks = options.hooks.value_or(FleetHooks{});
    hooks.claim = [this](const std::string& resource, bool wait) {
        return m_host.claim(resource, wait) == Claimed::Yes;
    };
    hooks.release = [this](const std::string& resource) { m_host.release(resource); };
    hooks.radio = m_radio;
    m_fleet = std::make_unique<Fleet>(std::move(hooks), [this] { fleetChanged(); });
    m_fleet->setSettings(globalSettings());
    m_fleet->start();
    m_fleet->setActive(true);
    log::info(kCategory, "the ESP32 touch box extension is looking for boards (firmware {})",
              m_bundle ? m_bundle->version : std::string("missing"));
}

Extension::~Extension()
{
    std::map<std::uint64_t, std::unique_ptr<Running>> jobs;
    {
        const std::scoped_lock lock(m_jobsMutex);
        m_stopping = true;
        for (auto& entry : m_jobs) {
            entry.second->thread.request_stop();
        }
        jobs.swap(m_jobs);
    }
    jobs.clear();
    m_fleet->stop();
    m_fleet.reset();
    log::setSink(nullptr);
}

std::optional<std::string> Extension::textSetting(std::string_view key) const
{
    const std::optional<std::string> raw = m_host.setting(key);
    if (!raw) {
        return std::nullopt;
    }
    const Json value = Json::parse(*raw, nullptr, false);
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

esp32::Esp32Settings Extension::globalSettings() const
{
    esp32::Esp32Settings settings;
    const std::string port = trimmed(textSetting("port").value_or(std::string()));
    settings.port = port.empty() ? std::string(esp32::kAutomaticPort) : port;
    settings.link = esp32::linkModeOf(textSetting("link").value_or(std::string()));
    settings.address = trimmed(textSetting("address").value_or(std::string()));
    settings.knownBoards = knownBoards();
    settings.radio = m_radio;
    return settings;
}

esp32::Esp32Settings Extension::settingsFor(std::string_view options) const
{
    esp32::Esp32Settings settings = globalSettings();
    const Json parsed = Json::parse(options, nullptr, false);
    if (!parsed.is_object()) {
        return settings;
    }
    const Json shared = parsed.contains("settings") ? parsed.at("settings") : Json::object();
    const Json own = parsed.contains("deviceSettings") ? parsed.at("deviceSettings") : Json::object();
    const auto valueOf = [&shared, &own](const char* key) -> std::optional<std::string> {
        for (const Json* part : {&own, &shared}) {
            if (part->is_object() && part->contains(key) && part->at(key).is_string()) {
                return part->at(key).get<std::string>();
            }
        }
        return std::nullopt;
    };
    if (const std::optional<std::string> port = valueOf("port")) {
        const std::string chosen = trimmed(*port);
        settings.port = chosen.empty() ? std::string(esp32::kAutomaticPort) : chosen;
    }
    if (const std::optional<std::string> link = valueOf("link")) {
        settings.link = esp32::linkModeOf(*link);
    }
    if (const std::optional<std::string> address = valueOf("address")) {
        settings.address = trimmed(*address);
    }
    return settings;
}

std::vector<esp32::KnownBoard> Extension::knownBoards() const
{
    const std::scoped_lock lock(m_mutex);
    return m_known;
}

std::vector<esp32::KnownBoard> Extension::loadKnown() const
{
    const std::optional<std::string> stored = m_host.storeGet(esp32::kBoardsStoreKey);
    return stored ? esp32::parseBoards(*stored) : std::vector<esp32::KnownBoard>{};
}

void Extension::rememberBoard(const esp32::KnownBoard& board)
{
    std::vector<esp32::KnownBoard> known;
    {
        const std::scoped_lock lock(m_mutex);
        m_known = esp32::rememberBoard(std::move(m_known), board);
        known = m_known;
    }
    saveKnown(known);
}

void Extension::forgetBoard(std::string_view id)
{
    std::vector<esp32::KnownBoard> known;
    {
        const std::scoped_lock lock(m_mutex);
        m_known = esp32::forgetBoard(std::move(m_known), id);
        known = m_known;
    }
    saveKnown(known);
}

void Extension::saveKnown(const std::vector<esp32::KnownBoard>& known)
{
    if (!m_host.storeSet(esp32::kBoardsStoreKey, esp32::writeBoards(known))) {
        log::warning(kCategory, "the Wi-Fi keys of the boards could not be kept");
    }
    esp32::Esp32Settings settings = m_fleet->settings();
    settings.knownBoards = known;
    m_fleet->setSettings(std::move(settings));
}

void Extension::fleetChanged()
{
    const std::vector<Board> boards = m_fleet->boards();
    const std::vector<esp32::SerialPortInfo> ports = m_fleet->ports();
    std::vector<std::pair<std::string, std::string>> events;
    std::optional<std::vector<esp32::KnownBoard>> known;
    bool listChanged = false;
    {
        const std::scoped_lock lock(m_mutex);
        for (const Board& board : boards) {
            const auto before = std::ranges::find(m_shown, board.id, &Board::id);
            if (before == m_shown.end()) {
                events.emplace_back("board.added", boardJson(board, m_bundle, m_language));
                listChanged = true;
            } else if (!sameShape(*before, board)) {
                events.emplace_back("board.changed", boardJson(board, m_bundle, m_language));
                listChanged = true;
            }
        }
        for (const Board& board : m_shown) {
            if (std::ranges::none_of(boards, [&board](const Board& now) { return now.id == board.id; })) {
                events.emplace_back("board.removed", idJson(board.id));
                listChanged = true;
            }
        }
        if (ports != m_shownPorts) {
            events.emplace_back("ports.changed", portsJson(ports));
            m_shownPorts = ports;
        }
        m_shown = boards;
        for (const Board& board : boards) {
            if (board.via != esp32::LinkMode::Serial || !board.info || board.status.wifi != GS_ESP32_WIFI_CONNECTED) {
                continue;
            }
            const std::string id = esp32::boardId(board.info->mac);
            const std::string address = esp32::ipv4Text(board.status.ipv4);
            const auto found = std::ranges::find(m_known, id, &esp32::KnownBoard::id);
            if (found != m_known.end() && (found->address != address || found->name != board.info->name)) {
                found->address = address;
                found->name = board.info->name;
                known = m_known;
            }
        }
    }
    if (listChanged) {
        static_cast<void>(m_host.devicesChanged(kProviderName));
    }
    for (const auto& [topic, json] : events) {
        emit(topic, json);
    }
    if (known) {
        saveKnown(*known);
    }
}

void Extension::emit(std::string_view topic, std::string_view json) const
{
    static_cast<void>(m_host.emit(topic, json));
}

std::int32_t Extension::jsonResult(gs_ext_result* result, std::string_view json) const
{
    if (result == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return m_host.setText(result, GS_EXT_TYPE_JSON, json);
}

std::int32_t Extension::flagResult(gs_ext_result* result, bool value) noexcept
{
    if (result == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    result->value = gs_ext_value{};
    result->value.type = GS_EXT_TYPE_BOOLEAN;
    result->value.flag = value ? 1 : 0;
    return GS_EXT_OK;
}

std::int32_t Extension::openTouch(const gs_ext_touch_open* info, void** session)
{
    if (info == nullptr || session == nullptr || (info->options == nullptr && info->options_size > 0)) {
        return GS_EXT_BAD_ARGUMENT;
    }
    const std::string_view options =
        info->options_size == 0 ? std::string_view("{}") : std::string_view(info->options, info->options_size);
    auto made = std::make_unique<Session>();
    made->owner = std::string(kSessionPrefix) + std::to_string(++m_sessions);
    const std::string touchBox = textIn(Json::parse(options, nullptr, false), "touchBox");
    if (!touchBox.empty()) {
        Taken taken = m_fleet->take(touchBox, made->owner, Holder::Session);
        if (taken.link == nullptr) {
            log::warning(kCategory, "the touch box {} is not connected or is in use", touchBox);
            return GS_EXT_FAILED;
        }
        made->resource = std::move(taken.resource);
        made->method.attach(std::move(taken.link));
    } else {
        const esp32::Esp32Settings settings = settingsFor(options);
        if (const std::optional<Board> found = m_fleet->matching(settings);
            found && found->connected && !found->held) {
            Taken taken = m_fleet->take(found->id, made->owner, Holder::Session);
            if (taken.link != nullptr) {
                made->resource = std::move(taken.resource);
                made->method.attach(std::move(taken.link));
            }
        }
        if (!made->method.opened()) {
            m_fleet->hold(made->owner, holdOf(settings));
            Expected<esp32::OpenedLink, esp32::OpenFailure> opened = esp32::openBoard(settings);
            if (!opened) {
                m_fleet->release(made->owner);
                log::warning(kCategory, "the ESP32 touch box did not open: {}", openFailureName(opened.error()));
                return GS_EXT_FAILED;
            }
            if (const std::string resource = resourceOf(opened->where, opened->mode); !resource.empty()) {
                if (!m_fleet->acquire(resource, false)) {
                    opened->link.reset();
                    m_fleet->release(made->owner);
                    log::warning(kCategory, "the ESP32 touch box on {} is in use by someone else", opened->where);
                    return GS_EXT_FAILED;
                }
                made->resource = resource;
            }
            m_fleet->hold(made->owner, Hold{.where = opened->where, .via = opened->mode, .every = false,
                                            .holder = Holder::Session});
            made->method.attach(std::move(opened->link));
        }
    }
    log::info(kCategory, "touches for {} go through the ESP32 touch box ({})",
              info->device != nullptr ? info->device : "the phone", made->owner);
    *session = made.release();
    return GS_EXT_OK;
}

void Extension::closeTouch(void* session)
{
    const std::unique_ptr<Session> owned(static_cast<Session*>(session));
    if (owned == nullptr) {
        return;
    }
    owned->method.close();
    m_fleet->release(owned->owner);
    if (!owned->resource.empty()) {
        m_fleet->unacquire(owned->resource);
    }
}

std::int32_t Extension::touchFrame(void* session, const gs_ext_touch_frame* frame)
{
    if (session == nullptr || frame == nullptr || (frame->count > 0 && frame->contacts == nullptr)) {
        return GS_EXT_BAD_ARGUMENT;
    }
    std::vector<esp32::Contact> contacts;
    contacts.reserve(frame->count);
    for (std::uint32_t index = 0; index < frame->count; ++index) {
        const gs_ext_contact& contact = frame->contacts[index];
        contacts.push_back(esp32::Contact{.pointerId = contact.id,
                                          .action = actionOf(contact.action),
                                          .x = pictureUnits(contact.x),
                                          .y = pictureUnits(contact.y)});
    }
    esp32::Esp32TouchMethod& method = static_cast<Session*>(session)->method;
    if (method.frame(contacts, static_cast<int>(frame->rotation & 3U))) {
        return GS_EXT_OK;
    }
    log::warning(kCategory, "a touch did not reach the board; the link is {}", linkStateName(method.state()));
    return GS_EXT_FAILED;
}

std::int32_t Extension::touchKey(void* session, const gs_ext_key* key)
{
    if (session == nullptr || key == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return static_cast<Session*>(session)->method.key(key->keycode, key->down != 0) ? GS_EXT_OK : GS_EXT_FAILED;
}

std::int32_t Extension::touchText(void* session, std::string_view typed)
{
    if (session == nullptr || typed.empty()) {
        return GS_EXT_BAD_ARGUMENT;
    }
    if (!std::ranges::all_of(typed, [](char character) { return static_cast<unsigned char>(character) < 0x80; })) {
        return GS_EXT_UNSUPPORTED;
    }
    return static_cast<Session*>(session)->method.text(typed) ? GS_EXT_OK : GS_EXT_FAILED;
}

std::int32_t Extension::touchMouse(void* session, const gs_ext_mouse* mouse)
{
    if (session == nullptr || mouse == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    const auto buttons =
        static_cast<std::uint8_t>(mouse->buttons & (GS_EXT_MOUSE_LEFT | GS_EXT_MOUSE_RIGHT | GS_EXT_MOUSE_MIDDLE));
    return static_cast<Session*>(session)->method.mouse(buttons, mouse->dx, mouse->dy, mouse->wheel) ? GS_EXT_OK
                                                                                                      : GS_EXT_FAILED;
}

std::int32_t Extension::touchGamepad(void* session, const gs_ext_gamepad* pad)
{
    if (session == nullptr || pad == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    const auto axis = [](std::int32_t value) {
        return static_cast<std::int16_t>(std::clamp<std::int32_t>(value, -32768, 32767));
    };
    const auto trigger = [](std::uint32_t value) { return static_cast<std::uint8_t>(std::min<std::uint32_t>(value, 255U)); };
    const esp32::Esp32Gamepad state{.buttons = pad->buttons,
                                    .hat = static_cast<std::uint8_t>(std::min<std::uint32_t>(pad->hat, GS_ESP32_HAT_CENTERED)),
                                    .lx = axis(pad->left_x),
                                    .ly = axis(pad->left_y),
                                    .rx = axis(pad->right_x),
                                    .ry = axis(pad->right_y),
                                    .lt = trigger(pad->left_trigger),
                                    .rt = trigger(pad->right_trigger)};
    return static_cast<Session*>(session)->method.gamepad(state) ? GS_EXT_OK : GS_EXT_FAILED;
}

std::int32_t Extension::listDevices(gs_ext_result* out)
{
    return jsonResult(out, devicesJson(m_fleet->boards(), m_language));
}

std::int32_t Extension::act(std::string_view device, std::string_view action)
{
    if (!m_fleet->board(device)) {
        return GS_EXT_BAD_ARGUMENT;
    }
    if (action == "tap") {
        return m_fleet->tap(device) ? GS_EXT_OK : GS_EXT_FAILED;
    }
    if (action == "disconnect") {
        m_fleet->dismiss(device);
        return GS_EXT_OK;
    }
    return GS_EXT_UNSUPPORTED;
}

std::int32_t Extension::boardsNode(gs_ext_result* result)
{
    return jsonResult(result, boardsJson(m_fleet->boards(), m_bundle, m_language));
}

std::int32_t Extension::portsNode(gs_ext_result* result)
{
    return jsonResult(result, portsJson(m_fleet->ports()));
}

std::int32_t Extension::firmwareNode(gs_ext_result* result)
{
    return jsonResult(result, firmwareJson(m_bundle));
}

std::int32_t Extension::languageNode(gs_ext_result* result)
{
    if (result == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return m_host.setText(result, GS_EXT_TYPE_STRING, languageCode(m_language));
}

std::int32_t Extension::problemNode(gs_ext_result* result)
{
    if (result == nullptr) {
        return GS_EXT_BAD_ARGUMENT;
    }
    std::string text;
    if (const std::optional<esp32::BoardTrouble> trouble = m_fleet->trouble()) {
        text = troubleText(*trouble, m_language);
    }
    if (const std::optional<esp32::OpenFailure> problem = m_fleet->problem(); text.empty() && problem) {
        text = openFailureText(*problem, m_fleet->settings().link, m_language);
    }
    return m_host.setText(result, GS_EXT_TYPE_STRING, text);
}

std::int32_t Extension::tapNode(std::string_view device, gs_ext_result* result)
{
    return flagResult(result, m_fleet->tap(device));
}

std::int32_t Extension::disconnectNode(std::string_view device, gs_ext_result* result)
{
    if (!m_fleet->board(device)) {
        return flagResult(result, false);
    }
    m_fleet->dismiss(device);
    return flagResult(result, true);
}

std::int32_t Extension::rescanNode(gs_ext_result* result)
{
    m_fleet->forgive();
    return flagResult(result, true);
}

std::int32_t Extension::bluetoothNode(gs_ext_result* result)
{
    return jsonResult(result, bluetoothJson());
}

std::string Extension::bluetoothJson() const
{
    const std::vector<esp32::KnownBoard> known = knownBoards();
    const std::vector<Board> boards = m_fleet->boards();
    Json found = Json::array();
    for (const esp32::SeenBoard& seen : m_radio->seen(esp32::kSeenRecently)) {
        const std::string address = esp32::bluetoothText(seen.address);
        const std::optional<esp32::KnownBoard> book = esp32::findBoard(known, seen.advert.id);
        const bool connected = std::ranges::any_of(boards, [&address](const Board& board) {
            return board.via == esp32::LinkMode::Bluetooth && board.connected && esp32::sameBluetooth(board.where, address);
        });
        found.push_back({{"address", address},
                         {"id", seen.advert.id},
                         {"name", seen.name.empty() ? std::string(pick(m_language, "ESP32 touch box", "ESP32 触控盒"))
                                                    : cutUtf8(seen.name, 64)},
                         {"chip", esp32::chipName(seen.advert.chipId)},
                         {"signal", seen.signal},
                         {"signalText", signalText(seen.signal, m_language)},
                         {"keyed", seen.advert.keyed},
                         {"busy", seen.advert.busy},
                         {"known", book.has_value()},
                         {"paired", book.has_value() && esp32::sameBluetooth(book->bluetooth, address)},
                         {"connected", connected}});
    }
    Json paired = Json::array();
    for (const esp32::KnownBoard& board : known) {
        if (!board.bluetooth.empty()) {
            paired.push_back({{"id", board.id}, {"name", board.name}, {"address", board.bluetooth}});
        }
    }
    const esp32::BluetoothState state = m_radio->state();
    Json value = Json::object();
    value["state"] = std::string(bluetoothStateName(state));
    value["stateText"] = bluetoothStateText(state, m_language);
    value["scanning"] = m_scans.load(std::memory_order_acquire) > 0;
    value["boards"] = std::move(found);
    value["paired"] = std::move(paired);
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void Extension::settingsChanged()
{
    m_fleet->setSettings(globalSettings());
}

std::int32_t Extension::claimWanted(std::string_view resource, std::string_view by)
{
    return m_fleet->yield(resource, by) ? GS_EXT_OK : GS_EXT_FAILED;
}

}
