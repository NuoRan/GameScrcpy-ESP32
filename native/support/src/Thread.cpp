#include "gs/support/Thread.h"

#include <windows.h>

#include <string>

namespace gs::platform {

void setCurrentThreadName(std::string_view name) noexcept
{
    try {
        const std::wstring wide(name.begin(), name.end());
        SetThreadDescription(GetCurrentThread(), wide.c_str());
    } catch (...) {
    }
}

}
