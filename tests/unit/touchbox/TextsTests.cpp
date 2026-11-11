#include <gs/touchbox/Texts.h>

#include <doctest/doctest.h>

#include <string>

namespace esp32 = gs::esp32;
namespace espflash = gs::espflash;
using gs::touchbox::Language;

TEST_CASE("ADR-0112: the texts say what is wrong with a board and which port a two-port board needs, in English and "
          "Chinese")
{
    esp32::BoardTrouble trouble;
    trouble.problem = esp32::BoardProblem::Silent;
    const std::string silent = gs::touchbox::troubleText(trouble, Language::English);
    CHECK(silent.find("Nothing answers") != std::string::npos);
    CHECK(silent.find("COM or UART") != std::string::npos);
    CHECK(gs::touchbox::troubleText(trouble, Language::Chinese).find("没有应答") != std::string::npos);
    trouble.problem = esp32::BoardProblem::Restarting;
    trouble.resetReason = "0x3 (RTC_SW_SYS_RST)";
    CHECK(gs::touchbox::troubleText(trouble, Language::English).find("keeps restarting") != std::string::npos);
    CHECK(gs::touchbox::troubleText(trouble, Language::English).find("0x3 (RTC_SW_SYS_RST)") != std::string::npos);
    CHECK(gs::touchbox::troubleText(trouble, Language::Chinese).find("0x3 (RTC_SW_SYS_RST)") != std::string::npos);
    trouble.resetReason.clear();
    CHECK(gs::touchbox::troubleText(trouble, Language::English) == "The board keeps restarting. Flash the firmware again.");
    trouble.problem = esp32::BoardProblem::DownloadMode;
    CHECK(gs::touchbox::troubleText(trouble, Language::English).find("download mode") != std::string::npos);
    trouble.problem = esp32::BoardProblem::WrongPort;
    CHECK(gs::touchbox::troubleText(trouble, Language::English).find("COM or UART") != std::string::npos);
    trouble.problem = esp32::BoardProblem::OldFirmware;
    CHECK(gs::touchbox::troubleText(trouble, Language::English).find("QineTouch") != std::string::npos);
    trouble.problem = esp32::BoardProblem::None;
    CHECK(gs::touchbox::troubleText(trouble, Language::English).empty());
}

TEST_CASE("ADR-0112: the parts line lists what the board reported in its HELLO and the live state after it")
{
    esp32::BoardInfo board;
    const esp32::BoardStatus unknown;
    CHECK(gs::touchbox::partsLine(board, unknown, Language::English).empty());
    board.parts.known = true;
    board.parts.usb = GS_ESP32_USB_READY;
    board.parts.usbProblem = GS_ESP32_PART_OK;
    board.parts.bleProblem = GS_ESP32_PART_UNSUPPORTED;
    board.parts.wifiProblem = GS_ESP32_PART_FAILED;
    const std::string line = gs::touchbox::partsLine(board, unknown, Language::English);
    CHECK(line == "USB to the phone: ready · Wi-Fi: did not start");
    CHECK(gs::touchbox::partsLine(board, unknown, Language::Chinese) == "接手机的 USB：就绪 · Wi-Fi：没能启动");

    esp32::BoardInfo starting;
    starting.parts.known = true;
    starting.parts.usb = GS_ESP32_USB_NONE;
    starting.parts.usbProblem = GS_ESP32_PART_OK;
    starting.parts.bleProblem = GS_ESP32_PART_UNSUPPORTED;
    starting.parts.wifiProblem = GS_ESP32_PART_OK;
    starting.wifi = GS_ESP32_WIFI_NONE;
    CHECK(gs::touchbox::partsLine(starting, {}, Language::English).find("USB to the phone: not started") !=
          std::string::npos);
    esp32::BoardStatus live;
    live.usb = GS_ESP32_USB_DETACHED;
    live.wifi = GS_ESP32_WIFI_OFF;
    CHECK(gs::touchbox::partsLine(starting, live, Language::English) == "USB to the phone: no phone · Wi-Fi: not set up");
    live.usb = GS_ESP32_USB_READY;
    CHECK(gs::touchbox::partsLine(starting, live, Language::English).find("USB to the phone: ready") != std::string::npos);
}

TEST_CASE("ADR-0112: the phone, Wi-Fi, flashing and opening texts follow the board and the language")
{
    esp32::BoardStatus status;
    status.usb = GS_ESP32_USB_READY;
    CHECK(gs::touchbox::phoneText(status, Language::English) == "Phone connected over USB");
    CHECK(gs::touchbox::phoneText(status, Language::Chinese) == "手机已通过 USB 连接");
    status.usb = GS_ESP32_USB_NONE;
    status.ble = GS_ESP32_BLE_CONNECTED;
    status.peer = "Pixel 8";
    CHECK(gs::touchbox::phoneText(status, Language::English) == "Bluetooth: Pixel 8");
    status.ble = GS_ESP32_BLE_NONE;
    CHECK(gs::touchbox::phoneText(status, Language::English).empty());

    status.wifi = GS_ESP32_WIFI_CONNECTED;
    status.ipv4 = {192, 168, 1, 20};
    CHECK(gs::touchbox::wifiText(status, Language::English) == "Wi-Fi 192.168.1.20");
    status.wifi = GS_ESP32_WIFI_FAILED;
    CHECK(gs::touchbox::wifiText(status, Language::English).find("check the network name") != std::string::npos);

    CHECK(gs::touchbox::flashStageText(espflash::FlashStage::Writing, Language::English) == "Writing the firmware…");
    CHECK(gs::touchbox::flashStageText(espflash::FlashStage::Writing, Language::Chinese) == "正在写入固件…");
    CHECK(gs::touchbox::flashFailureText(espflash::FlashFailure::NoDownloadMode, Language::English).find("BOOT") !=
          std::string::npos);
    CHECK(gs::touchbox::flashFailureText(espflash::FlashFailure::NoFirmware, Language::English).find("extension") !=
          std::string::npos);
    CHECK(gs::touchbox::openFailureText(esp32::OpenFailure::NoBoard, esp32::LinkMode::Wifi, Language::English)
              .find("Wi-Fi") != std::string::npos);
    CHECK(gs::touchbox::openFailureText(esp32::OpenFailure::NoBoard, esp32::LinkMode::Serial, Language::English)
              .find("serial port") != std::string::npos);
    CHECK(gs::touchbox::openFailureText(esp32::OpenFailure::Stopped, esp32::LinkMode::Serial, Language::English)
              .empty());

    CHECK(gs::touchbox::arg("%1 and %2", "keys", "mouse") == "keys and mouse");
    CHECK(gs::touchbox::arg("no marker", "x") == "no marker");
    CHECK(gs::touchbox::languageCode(Language::Chinese) == "zh_CN");
    CHECK(gs::touchbox::languageCode(Language::English) == "en");
}

TEST_CASE("ADR-0112: the Bluetooth texts say what to do when the radio, the pairing or the key is missing")
{
    const auto failure = [](esp32::OpenFailure problem, Language language) {
        return gs::touchbox::openFailureText(problem, esp32::LinkMode::Bluetooth, language);
    };
    CHECK(failure(esp32::OpenFailure::NoBoard, Language::English).find("Bluetooth") != std::string::npos);
    CHECK(failure(esp32::OpenFailure::NoKey, Language::English).find("USB") != std::string::npos);
    CHECK(failure(esp32::OpenFailure::NoAnswer, Language::English).find("1.1") != std::string::npos);
    CHECK(failure(esp32::OpenFailure::NoBluetooth, Language::Chinese) == "这台电脑没有低功耗蓝牙，或者蓝牙已关闭。");
    CHECK(failure(esp32::OpenFailure::NotPaired, Language::English).find("Pair one") != std::string::npos);
    CHECK(gs::touchbox::openFailureText(esp32::OpenFailure::NoKey, esp32::LinkMode::Wifi, Language::English)
              .find("Wi-Fi") != std::string::npos);

    CHECK(gs::touchbox::signalText(-60, Language::English) == "Signal good (-60 dBm)");
    CHECK(gs::touchbox::signalText(-72, Language::Chinese) == "信号一般（-72 dBm）");
    CHECK(gs::touchbox::slowBluetoothText(Language::Chinese) == "蓝牙延迟较高");
    CHECK(gs::touchbox::slowBluetoothText(Language::English) == "Bluetooth delay is high");
    CHECK(gs::touchbox::bluetoothStateText(esp32::BluetoothState::Off, Language::English).find("Windows") !=
          std::string::npos);
    CHECK_FALSE(gs::touchbox::bluetoothStateText(esp32::BluetoothState::Missing, Language::English).empty());
    CHECK(gs::touchbox::pairFailureText(esp32::PairOutcome::Refused, Language::English).find("Windows") !=
          std::string::npos);
    CHECK(gs::touchbox::pairFailureText(esp32::PairOutcome::Failed, Language::Chinese) == "配对没有成功，请再试一次。");
}
