#pragma once

#include "gs/touchbox/Fleet.h"
#include "gs/touchbox/Texts.h"

#include <gs/esp32/Port.h>
#include <gs/espflash/Firmware.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gs::touchbox {

inline constexpr std::string_view kProviderName = "boards";
inline constexpr std::string_view kTouchMethodName = "esp32";
inline constexpr std::string_view kBoardPanel = "board";
inline constexpr std::string_view kBoardIcon = "svg:icons/board.svg";
inline constexpr std::size_t kMostSummaryBytes = 200;

[[nodiscard]] std::string boardName(const Board& board, Language language);
[[nodiscard]] std::string boardSummary(const Board& board, Language language);
[[nodiscard]] std::string_view boardState(const Board& board) noexcept;
[[nodiscard]] std::string boardStateText(const Board& board, Language language);
[[nodiscard]] bool upgradable(const Board& board, const std::optional<espflash::FirmwareBundle>& bundle);
[[nodiscard]] std::string devicesJson(const std::vector<Board>& boards, Language language);
[[nodiscard]] std::string boardJson(const Board& board, const std::optional<espflash::FirmwareBundle>& bundle,
                                    Language language);
[[nodiscard]] std::string boardsJson(const std::vector<Board>& boards,
                                     const std::optional<espflash::FirmwareBundle>& bundle, Language language);
[[nodiscard]] std::string portsJson(const std::vector<esp32::SerialPortInfo>& ports);
[[nodiscard]] std::string firmwareJson(const std::optional<espflash::FirmwareBundle>& bundle);
[[nodiscard]] std::string cutUtf8(std::string text, std::size_t most);

}
