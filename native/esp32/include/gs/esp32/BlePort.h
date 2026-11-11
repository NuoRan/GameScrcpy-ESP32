#pragma once

#include "gs/esp32/Bluetooth.h"
#include "gs/esp32/Boards.h"
#include "gs/esp32/Port.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace gs::esp32 {

using BleConnect = std::function<std::unique_ptr<BleChannel>(BleChannelEvents events, BleFailure& failure)>;

class BlePort final : public Port
{
public:
    ~BlePort() override;
    BlePort(const BlePort&) = delete;
    BlePort& operator=(const BlePort&) = delete;

    [[nodiscard]] static std::unique_ptr<BlePort> open(const BleConnect& connect, std::vector<BoardKey> keys,
                                                       BleFailure& failure);

    [[nodiscard]] bool write(std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] std::size_t read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool setBaud(std::uint32_t baud) override;
    [[nodiscard]] bool setDtr(bool on) override;
    [[nodiscard]] bool setRts(bool on) override;
    void discardInput() override;
    void interrupt() override;
    [[nodiscard]] bool datagrams() const override;
    [[nodiscard]] bool gone() const override;

    [[nodiscard]] std::optional<BoardKey> matchedKey() const;
    [[nodiscard]] std::uint32_t droppedRecords() const;

private:
    struct Inbox;
    struct Impl;

    explicit BlePort(std::unique_ptr<Impl> impl);
    static void pump(Impl& impl, const std::stop_token& stop);

    std::unique_ptr<Impl> m_impl;
};

}
