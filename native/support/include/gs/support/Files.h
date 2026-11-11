#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace gs::platform {

[[nodiscard]] std::error_code readFile(const std::filesystem::path& file, std::size_t mostBytes,
                                       std::string& contents) noexcept;
[[nodiscard]] bool fileMissing(const std::error_code& error) noexcept;
[[nodiscard]] std::filesystem::path moduleFolder() noexcept;

}
