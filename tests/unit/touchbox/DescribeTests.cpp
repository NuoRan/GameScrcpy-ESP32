#include <gs/touchbox/Describe.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace esp32 = gs::esp32;
namespace espflash = gs::espflash;
using gs::touchbox::Board;
using gs::touchbox::Language;
using Json = nlohmann::json;

namespace {

Board readyBoard()
{
    Board board;
    board.id = "serial-com7";
    board.where = "COM7";
    board.kind = esp32::PortKind::Ch34x;
    board.connected = true;
    esp32::BoardInfo info;
    info.protocol = 2;
    info.firmware = "1.0.2";
    info.chipId = GS_ESP32_CHIP_ESP32S3;
    info.features = GS_ESP32_FEATURE_USB_HID | GS_ESP32_FEATURE_WIFI;
    info.mac = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
    info.name = "Desk box";
    board.info = info;
    board.status.usb = GS_ESP32_USB_READY;
    board.status.wifi = GS_ESP32_WIFI_OFF;
    return board;
}

std::vector<std::string> actionIds(const Json& device)
{
    std::vector<std::string> ids;
    for (const Json& action : device.at("actions")) {
        ids.push_back(action.at("id").get<std::string>());
    }
    return ids;
}

espflash::FirmwareBundle bundleOf(const std::string& version)
{
    espflash::FirmwareBundle bundle;
    bundle.version = version;
    bundle.images.push_back(espflash::FirmwareImage{.target = "esp32s3", .file = "gs-esp32-esp32s3.bin", .chipId = 9});
    return bundle;
}

}

TEST_CASE("ADR-0112: a board on the devices page is a touch box with its state, summary, actions and serial port")
{
    const Json devices = Json::parse(gs::touchbox::devicesJson({readyBoard()}, Language::English));
    REQUIRE(devices.is_array());
    REQUIRE(devices.size() == 1);
    const Json& device = devices[0];
    CHECK(device.at("id") == "serial-com7");
    CHECK(device.at("name") == "Desk box");
    CHECK(device.at("kind") == "touchBox");
    CHECK(device.at("state") == "ready");
    CHECK(device.at("stateText") == "");
    CHECK(device.at("touchMethod") == "esp32");
    CHECK(device.at("resource") == "serial:COM7");
    CHECK(device.at("icon") == "svg:icons/board.svg");
    CHECK(device.at("capture") == true);
    CHECK(device.at("summary") == "ESP32-S3 · firmware 1.0.2 · COM7 · Phone connected over USB");
    CHECK(actionIds(device) == std::vector<std::string>{"settings", "tap", "flash", "diagnose", "disconnect"});
    CHECK(device.at("actions")[0].at("panel") == "board");
    CHECK(device.at("actions")[2].at("job") == "flash");
    CHECK(device.at("actions")[3].at("job") == "diagnose");
    CHECK_FALSE(device.at("actions")[1].contains("panel"));
    CHECK_FALSE(device.at("actions")[1].contains("job"));

    Board session = readyBoard();
    session.connected = false;
    session.held = true;
    session.inSession = true;
    const Json used = Json::parse(gs::touchbox::devicesJson({session}, Language::English))[0];
    CHECK(used.at("state") == "busy");
    CHECK(used.at("stateText") == "In use by a mirror window");
    CHECK(actionIds(used) == std::vector<std::string>{"settings", "flash", "diagnose"});

    Board old = readyBoard();
    old.info.reset();
    old.legacy = true;
    const Json legacy = Json::parse(gs::touchbox::devicesJson({old}, Language::English))[0];
    CHECK(legacy.at("state") == "problem");
    CHECK(legacy.at("stateText") == "Old firmware");
    CHECK(legacy.at("name") == "ESP32 touch box");
    CHECK(legacy.at("summary") == "ESP32 with the old firmware · COM7");

    Board air = readyBoard();
    air.id = "wifi-192-168-1-20-4545";
    air.where = "192.168.1.20:4545";
    air.wifi = true;
    const Json wireless = Json::parse(gs::touchbox::devicesJson({air}, Language::English))[0];
    CHECK_FALSE(wireless.contains("resource"));
    CHECK(actionIds(wireless) == std::vector<std::string>{"settings", "tap", "diagnose", "disconnect"});
    CHECK(wireless.at("summary").get<std::string>().find("Wi-Fi 192.168.1.20:4545") != std::string::npos);

    Board taken = readyBoard();
    taken.connected = false;
    taken.yieldedTo = "Your logic";
    const Json elsewhere = Json::parse(gs::touchbox::devicesJson({taken}, Language::English))[0];
    CHECK(elsewhere.at("state") == "busy");
    CHECK(elsewhere.at("stateText") == "In use by Your logic");

    const Json chinese = Json::parse(gs::touchbox::devicesJson({session, old}, Language::Chinese));
    CHECK(chinese[0].at("stateText") == "正在被投屏窗口使用");
    CHECK(chinese[1].at("name") == "ESP32 触控盒");
    CHECK(chinese[0].at("actions")[0].at("title") == "设置");
}

TEST_CASE("ADR-0112: texts for the devices page stay within its limits and are cut on whole characters")
{
    Board board = readyBoard();
    board.info->name = std::string(40, 'n');
    board.status.usb = GS_ESP32_USB_NONE;
    board.status.ble = GS_ESP32_BLE_CONNECTED;
    board.status.peer = std::string(32, 'p');
    const Json device = Json::parse(gs::touchbox::devicesJson({board}, Language::Chinese))[0];
    CHECK(device.at("name").get<std::string>().size() <= 64);
    CHECK(device.at("summary").get<std::string>().size() <= gs::touchbox::kMostSummaryBytes);
    const std::string cut = gs::touchbox::cutUtf8("触控盒", 4);
    CHECK(cut == "触");
    CHECK(gs::touchbox::cutUtf8("abc", 10) == "abc");
}

TEST_CASE("ADR-0112: the panels get each board with its firmware, Wi-Fi, parts and whether an upgrade is there")
{
    Board board = readyBoard();
    board.roundTrip = std::chrono::microseconds{1500};
    board.status.wifi = GS_ESP32_WIFI_CONNECTED;
    board.status.ipv4 = {192, 168, 1, 20};
    const std::optional<espflash::FirmwareBundle> newer = bundleOf("1.0.3");
    const Json value = Json::parse(gs::touchbox::boardJson(board, newer, Language::English));
    CHECK(value.at("id") == "serial-com7");
    CHECK(value.at("chip") == "ESP32-S3");
    CHECK(value.at("target") == "esp32s3");
    CHECK(value.at("firmware") == "1.0.2");
    CHECK(value.at("boardName") == "Desk box");
    CHECK(value.at("mac") == "a1b2c3d4e5f6");
    CHECK(value.at("hasWifi") == true);
    CHECK(value.at("wifiConfigured") == true);
    CHECK(value.at("address") == "192.168.1.20");
    CHECK(value.at("wifiText") == "Wi-Fi 192.168.1.20");
    CHECK(value.at("phone") == "Phone connected over USB");
    CHECK(value.at("roundTripMs") == 1.5);
    CHECK(value.at("bundled") == "1.0.3");
    CHECK(value.at("upgradable") == true);
    CHECK(value.at("resource") == "serial:COM7");
    CHECK(value.at("features") == Json::array({"usbHid", "wifi"}));

    CHECK_FALSE(gs::touchbox::upgradable(board, bundleOf("1.0.2")));
    CHECK_FALSE(gs::touchbox::upgradable(board, std::nullopt));
    Board other = board;
    other.info->chipId = GS_ESP32_CHIP_ESP32C3;
    CHECK_FALSE(gs::touchbox::upgradable(other, newer));

    Board old = readyBoard();
    old.info.reset();
    old.legacy = true;
    old.trouble.problem = esp32::BoardProblem::OldFirmware;
    const Json legacy = Json::parse(gs::touchbox::boardJson(old, newer, Language::English));
    CHECK(legacy.at("chip").is_null());
    CHECK(legacy.at("firmware").is_null());
    CHECK(legacy.at("phone") == "");
    CHECK(legacy.at("problem").get<std::string>().find("QineTouch") != std::string::npos);

    const Json list = Json::parse(gs::touchbox::boardsJson({board, old}, newer, Language::English));
    REQUIRE(list.is_array());
    CHECK(list.size() == 2);
}

TEST_CASE("ADR-0112: the serial ports and the bundled firmware are described for the panels")
{
    const std::vector<esp32::SerialPortInfo> ports{
        esp32::SerialPortInfo{.name = "COM7", .description = "USB-SERIAL CH340 (COM7)", .kind = esp32::PortKind::Ch34x},
        esp32::SerialPortInfo{.name = "COM3", .description = "Communications Port", .kind = esp32::PortKind::Other},
        esp32::SerialPortInfo{.name = "COM9", .description = "", .kind = esp32::PortKind::EspressifUsb}};
    const Json listed = Json::parse(gs::touchbox::portsJson(ports));
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].at("title") == "USB-SERIAL CH340 (COM7)");
    CHECK(listed[0].at("kind") == "ch34x");
    CHECK(listed[0].at("likely") == true);
    CHECK(listed[1].at("title") == "COM3 · Communications Port");
    CHECK(listed[1].at("likely") == false);
    CHECK(listed[2].at("title") == "COM9");
    CHECK(listed[2].at("kind") == "espressif");

    CHECK(gs::touchbox::firmwareJson(std::nullopt) == "null");
    const Json firmware = Json::parse(gs::touchbox::firmwareJson(bundleOf("1.0.3")));
    CHECK(firmware.at("version") == "1.0.3");
    REQUIRE(firmware.at("targets").size() == 1);
    CHECK(firmware.at("targets")[0].at("target") == "esp32s3");
    CHECK(firmware.at("targets")[0].at("chip") == "ESP32-S3");
}
