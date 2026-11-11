#include "gs/esp32/Bluetooth.h"

#include "gs/esp32/Boards.h"

#include <algorithm>
#include <format>

namespace gs::esp32 {

namespace {

constexpr int kAddressDigits = 12;
constexpr int kAddressSeparators = 5;
constexpr int kGoodSignal = -67;
constexpr int kFairSignal = -80;

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

unsigned byteAt(std::uint64_t address, int shift) noexcept
{
    return static_cast<unsigned>((address >> shift) & 0xFFU);
}

}

std::string bluetoothText(std::uint64_t address)
{
    return std::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", byteAt(address, 40), byteAt(address, 32),
                       byteAt(address, 24), byteAt(address, 16), byteAt(address, 8), byteAt(address, 0));
}

std::optional<std::uint64_t> parseBluetooth(std::string_view text) noexcept
{
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    std::uint64_t address = 0;
    int digits = 0;
    int separators = 0;
    for (const char c : text) {
        const int value = hexValue(c);
        if (value >= 0) {
            if (++digits > kAddressDigits) {
                return std::nullopt;
            }
            address = (address << 4U) | static_cast<std::uint64_t>(value);
        } else if ((c == ':' || c == '-') && digits > 0 && digits % 2 == 0) {
            ++separators;
        } else {
            return std::nullopt;
        }
    }
    if (digits != kAddressDigits || (separators != 0 && separators != kAddressSeparators) || address == 0) {
        return std::nullopt;
    }
    return address;
}

std::string bluetoothResource(std::uint64_t address)
{
    return std::string(kBluetoothPrefix) + bluetoothText(address);
}

bool sameBluetooth(std::string_view a, std::string_view b) noexcept
{
    const std::optional<std::uint64_t> first = parseBluetooth(a);
    return first.has_value() && first == parseBluetooth(b);
}

std::optional<BoardAdvert> advertOf(std::span<const std::uint8_t> serviceData)
{
    if (serviceData.size() < kSerialService.size() + GS_ESP32_ADVERT_SIZE ||
        !std::ranges::equal(serviceData.first(kSerialService.size()), kSerialService)) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> data = serviceData.subspan(kSerialService.size());
    gs_esp32_advert advert{};
    if (!gs_esp32_decode_advert(data.data(), data.size(), &advert)) {
        return std::nullopt;
    }
    return BoardAdvert{.id = boardId(advert.id),
                       .chipId = advert.chip,
                       .keyed = (advert.flags & GS_ESP32_ADVERT_KEYED) != 0,
                       .busy = (advert.flags & GS_ESP32_ADVERT_BUSY) != 0};
}

SignalLevel signalLevel(int dbm) noexcept
{
    if (dbm >= kGoodSignal) {
        return SignalLevel::Good;
    }
    return dbm >= kFairSignal ? SignalLevel::Fair : SignalLevel::Weak;
}

}
