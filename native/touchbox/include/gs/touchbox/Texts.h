#pragma once

#include <gs/esp32/Diagnosis.h>
#include <gs/esp32/Link.h>
#include <gs/esp32/TouchMethod.h>
#include <gs/espflash/Flasher.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace gs::touchbox {

enum class Language : std::uint8_t { English, Chinese };

[[nodiscard]] Language systemLanguage() noexcept;
[[nodiscard]] std::string_view languageCode(Language language) noexcept;
[[nodiscard]] std::string_view pick(Language language, std::string_view english, std::string_view chinese) noexcept;
[[nodiscard]] std::string arg(std::string_view pattern, std::string_view first);
[[nodiscard]] std::string arg(std::string_view pattern, std::string_view first, std::string_view second);

[[nodiscard]] std::string openFailureText(esp32::OpenFailure failure, bool wifi, Language language);
[[nodiscard]] std::string flashFailureText(espflash::FlashFailure failure, Language language);
[[nodiscard]] std::string flashStageText(espflash::FlashStage stage, Language language);
[[nodiscard]] std::string phoneText(const esp32::BoardStatus& status, Language language);
[[nodiscard]] std::string wifiText(const esp32::BoardStatus& status, Language language);
[[nodiscard]] std::string troubleText(const esp32::BoardTrouble& trouble, Language language);
[[nodiscard]] std::string partsLine(const esp32::BoardInfo& board, const esp32::BoardStatus& status,
                                    Language language);

}
