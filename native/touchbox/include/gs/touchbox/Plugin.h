#pragma once

#include "gs/touchbox/Fleet.h"

#include <gamescrcpy_ext.h>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace gs::touchbox {

struct PluginOptions {
    std::optional<FleetHooks> hooks;
    std::filesystem::path folder;
    std::shared_ptr<esp32::BleRadio> radio;
};

[[nodiscard]] std::int32_t startPlugin(const gs_ext_host* host, const gs_ext_plugin** plugin,
                                       PluginOptions options = {});
[[nodiscard]] const gs_ext_plugin& pluginDescription() noexcept;

}
