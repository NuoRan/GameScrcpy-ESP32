#include "gs/touchbox/Describe.h"

#include <gs/support/Crypto.h>

#include <nlohmann/json.hpp>

#include <vector>

namespace gs::touchbox {

namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t kMostNameBytes = 64;

std::string dumped(const Json& value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string joined(const std::vector<std::string>& parts)
{
    std::string text;
    for (const std::string& part : parts) {
        if (part.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += " · ";
        }
        text += part;
    }
    return text;
}

std::string_view kindName(esp32::PortKind kind) noexcept
{
    switch (kind) {
    case esp32::PortKind::EspressifUsb:
        return "espressif";
    case esp32::PortKind::Ch34x:
        return "ch34x";
    case esp32::PortKind::Cp210x:
        return "cp210x";
    case esp32::PortKind::Other:
        break;
    }
    return "other";
}

std::string portTitle(const esp32::SerialPortInfo& port)
{
    if (port.description.empty()) {
        return port.name;
    }
    if (port.description.find(port.name) != std::string::npos) {
        return port.description;
    }
    return port.name + " · " + port.description;
}

Json featuresOf(std::uint32_t features)
{
    Json list = Json::array();
    const std::pair<std::uint32_t, const char*> named[] = {
        {GS_ESP32_FEATURE_USB_HID, "usbHid"},
        {GS_ESP32_FEATURE_USB_HIGH_SPEED, "usbHighSpeed"},
        {GS_ESP32_FEATURE_BLE_HID, "bleHid"},
        {GS_ESP32_FEATURE_WIFI, "wifi"},
        {GS_ESP32_FEATURE_USB_SERIAL_LINK, "usbSerialLink"},
    };
    for (const auto& [bit, name] : named) {
        if ((features & bit) != 0) {
            list.push_back(name);
        }
    }
    return list;
}

Json actionsOf(const Board& board, Language language)
{
    Json actions = Json::array();
    actions.push_back({{"id", "settings"}, {"title", pick(language, "Settings", "设置")}, {"panel", kBoardPanel}});
    if (board.connected && !board.held) {
        actions.push_back({{"id", "tap"}, {"title", pick(language, "Try it", "试一下")}});
    }
    if (!board.wifi) {
        actions.push_back({{"id", "flash"}, {"title", pick(language, "Flash firmware", "刷写固件")}, {"job", "flash"}});
    }
    actions.push_back({{"id", "diagnose"}, {"title", pick(language, "Diagnose", "诊断")}, {"job", "diagnose"}});
    if (!board.held) {
        actions.push_back({{"id", "disconnect"}, {"title", pick(language, "Disconnect", "断开")}});
    }
    return actions;
}

Json boardValue(const Board& board, const std::optional<espflash::FirmwareBundle>& bundle, Language language)
{
    Json value = Json::object();
    value["id"] = board.id;
    value["name"] = boardName(board, language);
    value["where"] = board.where;
    value["wifi"] = board.wifi;
    value["state"] = boardState(board);
    value["stateText"] = boardStateText(board, language);
    value["summary"] = boardSummary(board, language);
    value["connected"] = board.connected;
    value["legacy"] = board.legacy;
    value["inSession"] = board.inSession;
    value["flashing"] = board.flashing;
    value["held"] = board.held;
    value["yieldedTo"] = board.yieldedTo;
    value["resource"] = board.wifi ? std::string() : serialResource(board.where);
    if (board.info) {
        value["chip"] = esp32::chipName(board.info->chipId);
        value["target"] = esp32::chipTarget(board.info->chipId);
        value["firmware"] = board.info->firmware;
        value["boardName"] = board.info->name;
        value["mac"] = platform::toHex(board.info->mac);
        value["protocol"] = board.info->protocol;
        value["features"] = featuresOf(board.info->features);
        value["hasWifi"] = (board.info->features & GS_ESP32_FEATURE_WIFI) != 0;
        value["parts"] = partsLine(*board.info, board.status, language);
    } else {
        value["chip"] = nullptr;
        value["target"] = nullptr;
        value["firmware"] = nullptr;
        value["boardName"] = nullptr;
        value["mac"] = nullptr;
        value["protocol"] = nullptr;
        value["features"] = Json::array();
        value["hasWifi"] = false;
        value["parts"] = "";
    }
    value["phone"] = board.connected && !board.legacy ? phoneText(board.status, language) : std::string();
    value["wifiText"] = board.connected && !board.legacy ? wifiText(board.status, language) : std::string();
    value["wifiConfigured"] =
        board.status.wifi >= GS_ESP32_WIFI_CONNECTING && board.status.wifi <= GS_ESP32_WIFI_FAILED;
    value["address"] =
        board.status.wifi == GS_ESP32_WIFI_CONNECTED ? Json(esp32::ipv4Text(board.status.ipv4)) : Json(nullptr);
    if (board.roundTrip) {
        value["roundTripMs"] = static_cast<double>(board.roundTrip->count()) / 1000.0;
    } else {
        value["roundTripMs"] = nullptr;
    }
    value["problem"] = board.legacy ? troubleText(board.trouble, language) : std::string();
    value["bundled"] = bundle ? Json(bundle->version) : Json(nullptr);
    value["upgradable"] = upgradable(board, bundle);
    return value;
}

}

std::string cutUtf8(std::string text, std::size_t most)
{
    if (text.size() <= most) {
        return text;
    }
    std::size_t end = most;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    text.resize(end);
    return text;
}

std::string boardName(const Board& board, Language language)
{
    if (board.info && !board.info->name.empty()) {
        return cutUtf8(board.info->name, kMostNameBytes);
    }
    return std::string(pick(language, "ESP32 touch box", "ESP32 触控盒"));
}

std::string boardSummary(const Board& board, Language language)
{
    std::vector<std::string> parts;
    if (board.info) {
        parts.push_back(esp32::chipName(board.info->chipId));
        if (!board.info->firmware.empty()) {
            parts.push_back(arg(pick(language, "firmware %1", "固件 %1"), board.info->firmware));
        }
    } else if (board.legacy) {
        parts.emplace_back(pick(language, "ESP32 with the old firmware", "装着旧固件的 ESP32"));
    }
    parts.push_back(board.wifi ? arg(pick(language, "Wi-Fi %1", "Wi-Fi %1"), board.where) : board.where);
    if (board.connected && !board.legacy) {
        parts.push_back(phoneText(board.status, language));
    }
    return cutUtf8(joined(parts), kMostSummaryBytes);
}

std::string_view boardState(const Board& board) noexcept
{
    if (board.held || !board.yieldedTo.empty()) {
        return "busy";
    }
    if (!board.connected) {
        return "offline";
    }
    return board.legacy ? "problem" : "ready";
}

std::string boardStateText(const Board& board, Language language)
{
    if (board.inSession) {
        return std::string(pick(language, "In use by a mirror window", "正在被投屏窗口使用"));
    }
    if (board.flashing) {
        return std::string(pick(language, "Flashing firmware", "正在刷写固件"));
    }
    if (board.held) {
        return std::string(pick(language, "Busy with a task", "正在执行任务"));
    }
    if (!board.yieldedTo.empty()) {
        return arg(pick(language, "In use by %1", "正被 %1 使用"), board.yieldedTo);
    }
    if (!board.connected) {
        return std::string(pick(language, "Not connected", "未连接"));
    }
    if (board.legacy) {
        return std::string(pick(language, "Old firmware", "旧固件"));
    }
    return {};
}

bool upgradable(const Board& board, const std::optional<espflash::FirmwareBundle>& bundle)
{
    return bundle && board.info && espflash::imageForChip(*bundle, board.info->chipId) != nullptr &&
           espflash::compareVersions(bundle->version, board.info->firmware) > 0;
}

std::string devicesJson(const std::vector<Board>& boards, Language language)
{
    Json devices = Json::array();
    for (const Board& board : boards) {
        Json device = Json::object();
        device["id"] = board.id;
        device["name"] = boardName(board, language);
        device["kind"] = "touchBox";
        device["summary"] = boardSummary(board, language);
        device["state"] = boardState(board);
        device["stateText"] = cutUtf8(boardStateText(board, language), kMostSummaryBytes);
        device["icon"] = kBoardIcon;
        device["touchMethod"] = kTouchMethodName;
        if (!board.wifi) {
            device["resource"] = serialResource(board.where);
        }
        device["capture"] = true;
        device["actions"] = actionsOf(board, language);
        devices.push_back(std::move(device));
    }
    return dumped(devices);
}

std::string boardJson(const Board& board, const std::optional<espflash::FirmwareBundle>& bundle, Language language)
{
    return dumped(boardValue(board, bundle, language));
}

std::string boardsJson(const std::vector<Board>& boards, const std::optional<espflash::FirmwareBundle>& bundle,
                       Language language)
{
    Json list = Json::array();
    for (const Board& board : boards) {
        list.push_back(boardValue(board, bundle, language));
    }
    return dumped(list);
}

std::string portsJson(const std::vector<esp32::SerialPortInfo>& ports)
{
    Json list = Json::array();
    for (const esp32::SerialPortInfo& port : ports) {
        list.push_back({{"name", port.name},
                        {"title", portTitle(port)},
                        {"kind", kindName(port.kind)},
                        {"likely", esp32::probedAutomatically(port.kind)}});
    }
    return dumped(list);
}

std::string firmwareJson(const std::optional<espflash::FirmwareBundle>& bundle)
{
    if (!bundle) {
        return "null";
    }
    Json targets = Json::array();
    for (const espflash::FirmwareImage& image : bundle->images) {
        targets.push_back({{"target", image.target}, {"chip", esp32::chipName(static_cast<std::uint16_t>(image.chipId))}});
    }
    return dumped(Json{{"version", bundle->version}, {"targets", std::move(targets)}});
}

}
