#pragma once

#include <gamescrcpy_ext.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gs::touchbox {

enum class Claimed : std::uint8_t { Yes, Busy, Refused };

class Host
{
public:
    explicit Host(const gs_ext_host* host) noexcept;

    [[nodiscard]] bool supports(const char* feature) const noexcept;
    void log(std::uint32_t level, std::string_view text) const noexcept;
    [[nodiscard]] std::optional<std::string> storeGet(std::string_view key) const;
    [[nodiscard]] bool storeSet(std::string_view key, std::string_view json) const noexcept;
    [[nodiscard]] std::optional<std::string> setting(std::string_view key, std::string_view phone = {}) const;
    [[nodiscard]] bool emit(std::string_view topic, std::string_view json) const noexcept;
    [[nodiscard]] bool devicesChanged(std::string_view provider) const noexcept;
    [[nodiscard]] Claimed claim(std::string_view resource, bool wait) const noexcept;
    void release(std::string_view resource) const noexcept;
    void progress(std::uint64_t job, double fraction, std::string_view text) const noexcept;
    void jobLog(std::uint64_t job, std::uint32_t level, std::string_view text) const noexcept;
    void finish(std::uint64_t job, std::string_view json) const noexcept;
    void fail(std::uint64_t job, std::string_view text) const noexcept;
    [[nodiscard]] std::int32_t setText(gs_ext_result* result, std::uint32_t type, std::string_view text) const noexcept;
    [[nodiscard]] std::int32_t setError(gs_ext_result* result, std::string_view text) const noexcept;

private:
    [[nodiscard]] bool reaches(std::size_t offset, std::size_t size) const noexcept;

    const gs_ext_host* m_host;
};

}
