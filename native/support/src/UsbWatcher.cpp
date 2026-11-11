#include "gs/support/UsbWatcher.h"

#include <windows.h>

#include <cfgmgr32.h>

#include <cstddef>
#include <cwctype>
#include <string_view>
#include <utility>

namespace gs::platform {

namespace {

bool startsWithIgnoringCase(const wchar_t* text, std::wstring_view prefix) noexcept
{
    if (text == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (text[index] == L'\0' || std::towupper(text[index]) != std::towupper(prefix[index])) {
            return false;
        }
    }
    return true;
}

}

struct UsbWatcher::Impl {
    Changed changed;
    HCMNOTIFICATION handle = nullptr;

    static DWORD CALLBACK onEvent(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                  PCM_NOTIFY_EVENT_DATA data, DWORD)
    {
        if ((action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) &&
            data != nullptr && data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE &&
            startsWithIgnoringCase(data->u.DeviceInterface.SymbolicLink, L"\\\\?\\USB#")) {
            const auto* impl = static_cast<const Impl*>(context);
            if (impl->changed) {
                impl->changed();
            }
        }
        return ERROR_SUCCESS;
    }
};

UsbWatcher::UsbWatcher(Changed changed)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->changed = std::move(changed);
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    if (CM_Register_Notification(&filter, m_impl.get(), &Impl::onEvent, &m_impl->handle) != CR_SUCCESS) {
        m_impl->handle = nullptr;
    }
}

UsbWatcher::~UsbWatcher()
{
    if (m_impl->handle != nullptr) {
        CM_Unregister_Notification(m_impl->handle);
    }
}

bool UsbWatcher::started() const noexcept
{
    return m_impl->handle != nullptr;
}

}
