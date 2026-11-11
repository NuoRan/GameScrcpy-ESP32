#pragma once

#include <functional>
#include <memory>

namespace gs::platform {

class UsbWatcher
{
public:
    using Changed = std::function<void()>;

    explicit UsbWatcher(Changed changed);
    ~UsbWatcher();
    UsbWatcher(const UsbWatcher&) = delete;
    UsbWatcher& operator=(const UsbWatcher&) = delete;

    [[nodiscard]] bool started() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}
