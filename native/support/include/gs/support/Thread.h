#pragma once

#include <string_view>

namespace gs::platform {

void setCurrentThreadName(std::string_view name) noexcept;

}
