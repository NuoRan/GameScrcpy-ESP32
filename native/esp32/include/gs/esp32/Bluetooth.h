#pragma once

#include <gs_esp32_protocol.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace gs::esp32 {

using BluetoothUuid = std::array<std::uint8_t, 16>;

inline constexpr BluetoothUuid kSerialService{GS_ESP32_BLE_SERVICE_UUID128};
inline constexpr BluetoothUuid kSerialReceive{GS_ESP32_BLE_RX_UUID128};
inline constexpr BluetoothUuid kSerialSend{GS_ESP32_BLE_TX_UUID128};
inline constexpr std::string_view kBluetoothPrefix = "ble:";
inline constexpr std::chrono::milliseconds kSeenRecently{30000};

[[nodiscard]] std::string bluetoothText(std::uint64_t address);
[[nodiscard]] std::optional<std::uint64_t> parseBluetooth(std::string_view text) noexcept;
[[nodiscard]] std::string bluetoothResource(std::uint64_t address);
[[nodiscard]] bool sameBluetooth(std::string_view a, std::string_view b) noexcept;

struct BoardAdvert {
    std::string id;
    std::uint16_t chipId = 0;
    bool keyed = false;
    bool busy = false;

    friend bool operator==(const BoardAdvert&, const BoardAdvert&) = default;
};

[[nodiscard]] std::optional<BoardAdvert> advertOf(std::span<const std::uint8_t> serviceData);

enum class SignalLevel : std::uint8_t { Weak, Fair, Good };

[[nodiscard]] SignalLevel signalLevel(int dbm) noexcept;

struct SeenBoard {
    std::uint64_t address = 0;
    std::string name;
    BoardAdvert advert;
    int signal = 0;
    std::chrono::steady_clock::time_point at{};

    friend bool operator==(const SeenBoard&, const SeenBoard&) = default;
};

enum class BluetoothState : std::uint8_t { Unknown, Missing, Off, Ready };

enum class PairOutcome : std::uint8_t { Paired, NoRadio, NotFound, Refused, Failed };

enum class BleFailure : std::uint8_t { None, NoRadio, NotFound, NoService, NotPaired, Refused, Unreachable };

struct BleChannelEvents {
    std::function<void(std::span<const std::uint8_t> bytes)> received;
    std::function<void()> lost;
};

class BleChannel
{
public:
    virtual ~BleChannel() = default;

    [[nodiscard]] virtual std::size_t chunk() const = 0;
    [[nodiscard]] virtual bool send(std::span<const std::uint8_t> bytes) = 0;
};

class BleRadio
{
public:
    virtual ~BleRadio() = default;

    [[nodiscard]] virtual BluetoothState state() = 0;
    virtual void watch(bool on) = 0;
    [[nodiscard]] virtual std::vector<SeenBoard> seen(std::chrono::milliseconds within) = 0;
    [[nodiscard]] virtual bool find(std::uint64_t address, std::chrono::milliseconds timeout,
                                    const std::stop_token& stop) = 0;
    [[nodiscard]] virtual bool linked(std::uint64_t address) = 0;
    [[nodiscard]] virtual bool paired(std::uint64_t address) = 0;
    [[nodiscard]] virtual PairOutcome pair(std::uint64_t address) = 0;
    [[nodiscard]] virtual bool unpair(std::uint64_t address) = 0;
    [[nodiscard]] virtual std::unique_ptr<BleChannel> open(std::uint64_t address, BleChannelEvents events,
                                                           BleFailure& failure) = 0;
};

[[nodiscard]] std::shared_ptr<BleRadio> systemBluetooth();

}
