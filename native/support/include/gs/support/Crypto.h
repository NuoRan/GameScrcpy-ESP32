#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace gs::platform {

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> data);

void randomBytes(std::span<std::uint8_t> out);

[[nodiscard]] std::string toHex(std::span<const std::uint8_t> bytes);

}
