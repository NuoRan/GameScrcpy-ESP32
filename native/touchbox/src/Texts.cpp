#include "gs/touchbox/Texts.h"

#include <windows.h>

#include <vector>

namespace gs::touchbox {

namespace {

std::string replaced(std::string_view pattern, std::string_view marker, std::string_view value)
{
    std::string text(pattern);
    const std::size_t at = text.find(marker);
    if (at != std::string::npos) {
        text.replace(at, marker.size(), value);
    }
    return text;
}

std::string joined(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string text;
    for (const std::string& part : parts) {
        if (!text.empty()) {
            text += separator;
        }
        text += part;
    }
    return text;
}

std::string partProblem(std::uint8_t problem, Language language)
{
    switch (problem) {
    case GS_ESP32_PART_FAILED:
        return std::string(pick(language, "did not start", "没能启动"));
    case GS_ESP32_PART_PENDING:
        return std::string(pick(language, "starting", "正在启动"));
    case GS_ESP32_PART_UNSUPPORTED:
        return std::string(pick(language, "not on this board", "这块板子没有"));
    default:
        return {};
    }
}

}

Language systemLanguage() noexcept
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? Language::Chinese : Language::English;
}

std::string_view languageCode(Language language) noexcept
{
    return language == Language::Chinese ? "zh_CN" : "en";
}

std::string_view pick(Language language, std::string_view english, std::string_view chinese) noexcept
{
    return language == Language::Chinese ? chinese : english;
}

std::string arg(std::string_view pattern, std::string_view first)
{
    return replaced(pattern, "%1", first);
}

std::string arg(std::string_view pattern, std::string_view first, std::string_view second)
{
    return replaced(replaced(pattern, "%1", first), "%2", second);
}

std::string openFailureText(esp32::OpenFailure failure, bool wifi, Language language)
{
    switch (failure) {
    case esp32::OpenFailure::NoBoard:
        if (wifi) {
            return std::string(pick(language,
                                    "No ESP32 touch box answered over Wi-Fi. Check that the board and this computer "
                                    "are on the same network, or enter the board's address.",
                                    "没有 ESP32 触控盒通过 Wi-Fi 应答。检查开发板与这台电脑是否在同一个网络，或者填写开发板的地址。"));
        }
        return std::string(pick(language,
                                "No ESP32 touch box was found. Plug the board's serial port into this computer, or "
                                "choose its port.",
                                "没有找到 ESP32 触控盒。把开发板的串口接到这台电脑，或者手动选择它的串口。"));
    case esp32::OpenFailure::PortMissing:
        return std::string(pick(language, "That serial port is not there. Check the cable or choose another port.",
                                "这个串口不存在。检查数据线，或者换一个串口。"));
    case esp32::OpenFailure::PortBusy:
        return std::string(pick(language,
                                "Another program is using the serial port. A mirrored phone may be using the board "
                                "right now.",
                                "串口正被别的程序占用。可能有一台正在投屏的手机在用这块开发板。"));
    case esp32::OpenFailure::NoAnswer:
        return std::string(pick(language,
                                "The board does not answer. On an ESP32-S3, connect the computer to the port marked "
                                "COM or UART; the other USB port goes to the phone. If it still does not answer, flash "
                                "the firmware.",
                                "开发板没有应答。ESP32-S3 要把电脑接到标着 COM 或 UART 的口，另一个 USB 口接手机。还是没有应答就刷一次固件。"));
    case esp32::OpenFailure::NoKey:
        return std::string(pick(language,
                                "Set up Wi-Fi once while the board is on the serial port; then it can be reached over "
                                "Wi-Fi.",
                                "先用串口连上开发板设置一次 Wi-Fi，之后就能通过 Wi-Fi 连接。"));
    case esp32::OpenFailure::BadAddress:
        return std::string(pick(language, "That address is not valid.", "这个地址无效。"));
    case esp32::OpenFailure::Stopped:
        break;
    }
    return {};
}

std::string flashFailureText(espflash::FlashFailure failure, Language language)
{
    switch (failure) {
    case espflash::FlashFailure::NoFirmware:
        return std::string(pick(language, "This copy of the extension has no ESP32 firmware with it.",
                                "这份扩展没有附带 ESP32 固件。"));
    case espflash::FlashFailure::NoImageForChip:
        return std::string(pick(language, "The bundled firmware has nothing for this chip.",
                                "附带的固件里没有适用于这款芯片的版本。"));
    case espflash::FlashFailure::ImageMissing:
    case espflash::FlashFailure::ImageCorrupt:
        return std::string(pick(language, "The bundled firmware file is damaged. Install the extension again.",
                                "附带的固件文件已损坏，请重新安装这个扩展。"));
    case espflash::FlashFailure::PortMissing:
        return std::string(pick(language, "That serial port is not there. Check the cable or choose another port.",
                                "这个串口不存在。检查数据线，或者换一个串口。"));
    case espflash::FlashFailure::PortBusy:
        return std::string(pick(language, "Another program is using the serial port. Close it, then try again.",
                                "串口正被别的程序占用。关掉它后再试一次。"));
    case espflash::FlashFailure::NoDownloadMode:
        return std::string(pick(language,
                                "Could not enter download mode: hold BOOT while plugging in the cable, then try again.",
                                "没有进入下载模式：按住 BOOT 再插上数据线，然后再试一次。"));
    case espflash::FlashFailure::UnknownChip:
        return std::string(pick(language, "The chip on this board is not one the extension supports.",
                                "这个扩展不支持这块开发板上的芯片。"));
    case espflash::FlashFailure::Refused:
    case espflash::FlashFailure::WriteFailed:
        return std::string(pick(language,
                                "Writing the firmware failed. Try again; if it keeps failing, try another cable.",
                                "写入固件失败。请再试一次；一直失败的话换一根数据线。"));
    case espflash::FlashFailure::VerifyFailed:
        return std::string(pick(language, "The written firmware does not match. Flash it again.",
                                "写入的固件校验不一致，请重新刷写。"));
    case espflash::FlashFailure::Lost:
        return std::string(pick(language,
                                "The board stopped answering while flashing. Check the cable, then flash again.",
                                "刷写时开发板没有响应了。检查数据线后重新刷写。"));
    case espflash::FlashFailure::Cancelled:
        return std::string(pick(language, "Flashing was cancelled.", "已取消刷写。"));
    }
    return {};
}

std::string flashStageText(espflash::FlashStage stage, Language language)
{
    switch (stage) {
    case espflash::FlashStage::Preparing:
        return std::string(pick(language, "Preparing…", "正在准备…"));
    case espflash::FlashStage::Connecting:
        return std::string(pick(language, "Putting the board into download mode…", "正在让开发板进入下载模式…"));
    case espflash::FlashStage::Detecting:
        return std::string(pick(language, "Identifying the chip…", "正在识别芯片…"));
    case espflash::FlashStage::Erasing:
        return std::string(pick(language, "Erasing…", "正在擦除…"));
    case espflash::FlashStage::Writing:
        return std::string(pick(language, "Writing the firmware…", "正在写入固件…"));
    case espflash::FlashStage::Verifying:
        return std::string(pick(language, "Checking what was written…", "正在校验写入的内容…"));
    case espflash::FlashStage::Restarting:
        return std::string(pick(language, "Restarting the board…", "正在重启开发板…"));
    case espflash::FlashStage::Done:
        return std::string(pick(language, "Done", "完成"));
    }
    return {};
}

std::string phoneText(const esp32::BoardStatus& status, Language language)
{
    switch (status.usb) {
    case GS_ESP32_USB_READY:
        return std::string(pick(language, "Phone connected over USB", "手机已通过 USB 连接"));
    case GS_ESP32_USB_SUSPENDED:
        return std::string(pick(language, "Phone's USB is asleep", "手机的 USB 处于休眠"));
    case GS_ESP32_USB_DETACHED:
        return std::string(pick(language, "Waiting for the phone on USB", "等待手机通过 USB 连接"));
    default:
        break;
    }
    switch (status.ble) {
    case GS_ESP32_BLE_CONNECTED:
        return status.peer.empty() ? std::string(pick(language, "Phone connected over Bluetooth", "手机已通过蓝牙连接"))
                                   : arg(pick(language, "Bluetooth: %1", "蓝牙：%1"), status.peer);
    case GS_ESP32_BLE_ADVERTISING:
        return std::string(pick(language, "Waiting for the phone to pair over Bluetooth", "等待手机通过蓝牙配对"));
    case GS_ESP32_BLE_IDLE:
        return std::string(pick(language, "Bluetooth is off", "蓝牙已关闭"));
    default:
        break;
    }
    return {};
}

std::string wifiText(const esp32::BoardStatus& status, Language language)
{
    switch (status.wifi) {
    case GS_ESP32_WIFI_OFF:
        return std::string(pick(language, "Wi-Fi not set up", "未设置 Wi-Fi"));
    case GS_ESP32_WIFI_CONNECTING:
        return std::string(pick(language, "Connecting to Wi-Fi…", "正在连接 Wi-Fi…"));
    case GS_ESP32_WIFI_CONNECTED:
        return arg(pick(language, "Wi-Fi %1", "Wi-Fi %1"), esp32::ipv4Text(status.ipv4));
    case GS_ESP32_WIFI_FAILED:
        return std::string(pick(language, "Wi-Fi could not connect: check the network name and password",
                                "Wi-Fi 连不上：请检查网络名称和密码"));
    default:
        break;
    }
    return {};
}

std::string troubleText(const esp32::BoardTrouble& trouble, Language language)
{
    switch (trouble.problem) {
    case esp32::BoardProblem::Silent:
        return std::string(pick(language,
                                "Nothing answers on this port. On an ESP32-S3, connect the computer to the port marked "
                                "COM or UART; the other USB port goes to the phone.",
                                "这个口上没有应答。ESP32-S3 要把电脑接到标着 COM 或 UART 的口，另一个 USB 口接手机。"));
    case esp32::BoardProblem::Restarting:
        if (!trouble.resetReason.empty()) {
            return arg(pick(language, "The board keeps restarting (reset reason %1). Flash the firmware again.",
                            "开发板一直在重启（复位原因 %1）。重新刷一次固件。"),
                       trouble.resetReason);
        }
        return std::string(pick(language, "The board keeps restarting. Flash the firmware again.",
                                "开发板一直在重启。重新刷一次固件。"));
    case esp32::BoardProblem::DownloadMode:
        return std::string(pick(language, "The board is in download mode. Press RST on the board, then connect again.",
                                "开发板处在下载模式。按一下板子上的 RST，再重新连接。"));
    case esp32::BoardProblem::WrongPort:
        return std::string(pick(language,
                                "The bytes on this port do not look like the ESP32 touch box: the port or its speed is "
                                "wrong, or the board restarts before it can talk. On an ESP32-S3, connect the computer "
                                "to the port marked COM or UART.",
                                "这个口上收到的数据不像 ESP32 触控盒：口或波特率不对，或者开发板还没来得及应答就重启了。ESP32-S3 要把电脑接到标着 COM 或 UART 的口。"));
    case esp32::BoardProblem::OldFirmware:
        return std::string(pick(language,
                                "The board runs the old QineTouch firmware: only touches can be sent. Flash the bundled "
                                "firmware to use the keys, the mouse, the gamepad and Wi-Fi.",
                                "开发板上是旧版 QineTouch 固件，只能发触摸。刷入自带的固件才能用键盘、鼠标、手柄和 Wi-Fi。"));
    case esp32::BoardProblem::None:
        break;
    }
    return {};
}

std::string partsLine(const esp32::BoardInfo& board, const esp32::BoardStatus& status, Language language)
{
    if (!board.parts.known) {
        return {};
    }
    const std::uint8_t usb = status.usb != GS_ESP32_USB_NONE ? status.usb : board.parts.usb;
    const std::uint8_t ble = status.ble != GS_ESP32_BLE_NONE ? status.ble : board.parts.ble;
    const std::uint8_t wifi = status.wifi != GS_ESP32_WIFI_NONE ? status.wifi : board.wifi;
    std::vector<std::string> parts;
    if (board.parts.usbProblem != GS_ESP32_PART_UNSUPPORTED) {
        std::string state = partProblem(board.parts.usbProblem, language);
        if (state.empty()) {
            switch (usb) {
            case GS_ESP32_USB_READY:
                state = pick(language, "ready", "就绪");
                break;
            case GS_ESP32_USB_SUSPENDED:
                state = pick(language, "asleep", "休眠中");
                break;
            case GS_ESP32_USB_DETACHED:
                state = pick(language, "no phone", "没接手机");
                break;
            default:
                state = pick(language, "not started", "未启动");
                break;
            }
        }
        parts.push_back(arg(pick(language, "USB to the phone: %1", "接手机的 USB：%1"), state));
    }
    if (board.parts.bleProblem != GS_ESP32_PART_UNSUPPORTED) {
        std::string state = partProblem(board.parts.bleProblem, language);
        if (state.empty()) {
            switch (ble) {
            case GS_ESP32_BLE_CONNECTED:
                state = pick(language, "phone connected", "手机已连接");
                break;
            case GS_ESP32_BLE_ADVERTISING:
                state = pick(language, "waiting for the phone", "等待手机");
                break;
            case GS_ESP32_BLE_IDLE:
                state = pick(language, "off", "关闭");
                break;
            default:
                state = pick(language, "not started", "未启动");
                break;
            }
        }
        parts.push_back(arg(pick(language, "Bluetooth: %1", "蓝牙：%1"), state));
    }
    if (board.parts.wifiProblem != GS_ESP32_PART_UNSUPPORTED) {
        std::string state = partProblem(board.parts.wifiProblem, language);
        if (state.empty()) {
            switch (wifi) {
            case GS_ESP32_WIFI_CONNECTED:
                state = pick(language, "connected", "已连接");
                break;
            case GS_ESP32_WIFI_CONNECTING:
                state = pick(language, "connecting", "正在连接");
                break;
            case GS_ESP32_WIFI_FAILED:
                state = pick(language, "could not connect", "连不上");
                break;
            case GS_ESP32_WIFI_OFF:
                state = pick(language, "not set up", "未设置");
                break;
            default:
                state = pick(language, "not started", "未启动");
                break;
            }
        }
        parts.push_back(arg(pick(language, "Wi-Fi: %1", "Wi-Fi：%1"), state));
    }
    return joined(parts, " · ");
}

}
