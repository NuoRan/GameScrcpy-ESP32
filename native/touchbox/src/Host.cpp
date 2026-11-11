#include "gs/touchbox/Host.h"

namespace gs::touchbox {

#define GS_HOST_HAS(member) reaches(offsetof(gs_ext_host, member), sizeof(m_host->member))

Host::Host(const gs_ext_host* host) noexcept
    : m_host(host)
{
}

bool Host::reaches(std::size_t offset, std::size_t size) const noexcept
{
    return m_host != nullptr && m_host->size >= offset + size;
}

bool Host::supports(const char* feature) const noexcept
{
    return GS_HOST_HAS(supports) && m_host->supports != nullptr && m_host->supports(m_host->context, feature) != 0;
}

void Host::log(std::uint32_t level, std::string_view text) const noexcept
{
    if (GS_HOST_HAS(log) && m_host->log != nullptr) {
        m_host->log(m_host->context, level, text.data(), text.size());
    }
}

std::optional<std::string> Host::storeGet(std::string_view key) const
{
    if (!GS_HOST_HAS(store_get) || m_host->store_get == nullptr) {
        return std::nullopt;
    }
    gs_ext_result result{};
    if (m_host->store_get(m_host->context, key.data(), key.size(), &result) != GS_EXT_OK ||
        result.value.type != GS_EXT_TYPE_JSON || result.value.text == nullptr) {
        return std::nullopt;
    }
    return std::string(result.value.text, result.value.size);
}

bool Host::storeSet(std::string_view key, std::string_view json) const noexcept
{
    return GS_HOST_HAS(store_set) && m_host->store_set != nullptr &&
           m_host->store_set(m_host->context, key.data(), key.size(), json.data(), json.size()) == GS_EXT_OK;
}

std::optional<std::string> Host::setting(std::string_view key, std::string_view phone) const
{
    if (!GS_HOST_HAS(setting_get) || m_host->setting_get == nullptr || !supports("settings")) {
        return std::nullopt;
    }
    gs_ext_result result{};
    if (m_host->setting_get(m_host->context, key.data(), key.size(), phone.data(), phone.size(), &result) != GS_EXT_OK ||
        result.value.type != GS_EXT_TYPE_JSON || result.value.text == nullptr) {
        return std::nullopt;
    }
    return std::string(result.value.text, result.value.size);
}

bool Host::emit(std::string_view topic, std::string_view json) const noexcept
{
    return GS_HOST_HAS(emit_event) && m_host->emit_event != nullptr &&
           m_host->emit_event(m_host->context, topic.data(), topic.size(), json.data(), json.size()) == GS_EXT_OK;
}

bool Host::devicesChanged(std::string_view provider) const noexcept
{
    return GS_HOST_HAS(devices_changed) && m_host->devices_changed != nullptr &&
           m_host->devices_changed(m_host->context, provider.data(), provider.size()) == GS_EXT_OK;
}

Claimed Host::claim(std::string_view resource, bool wait) const noexcept
{
    if (!GS_HOST_HAS(claim) || m_host->claim == nullptr || !supports("claims")) {
        return Claimed::Yes;
    }
    const std::int32_t answer =
        m_host->claim(m_host->context, resource.data(), resource.size(), wait ? GS_EXT_CLAIM_WAIT : 0U);
    if (answer == GS_EXT_OK || answer == GS_EXT_UNSUPPORTED) {
        return Claimed::Yes;
    }
    return answer == GS_EXT_BUSY ? Claimed::Busy : Claimed::Refused;
}

void Host::release(std::string_view resource) const noexcept
{
    if (GS_HOST_HAS(release) && m_host->release != nullptr && supports("claims")) {
        static_cast<void>(m_host->release(m_host->context, resource.data(), resource.size()));
    }
}

void Host::progress(std::uint64_t job, double fraction, std::string_view text) const noexcept
{
    if (GS_HOST_HAS(job_progress) && m_host->job_progress != nullptr) {
        static_cast<void>(m_host->job_progress(m_host->context, job, fraction, text.data(), text.size()));
    }
}

void Host::jobLog(std::uint64_t job, std::uint32_t level, std::string_view text) const noexcept
{
    if (GS_HOST_HAS(job_log) && m_host->job_log != nullptr) {
        static_cast<void>(m_host->job_log(m_host->context, job, level, text.data(), text.size()));
    }
}

void Host::finish(std::uint64_t job, std::string_view json) const noexcept
{
    if (GS_HOST_HAS(job_finish) && m_host->job_finish != nullptr) {
        static_cast<void>(m_host->job_finish(m_host->context, job, json.data(), json.size()));
    }
}

void Host::fail(std::uint64_t job, std::string_view text) const noexcept
{
    if (GS_HOST_HAS(job_fail) && m_host->job_fail != nullptr) {
        static_cast<void>(m_host->job_fail(m_host->context, job, text.data(), text.size()));
    }
}

std::int32_t Host::setText(gs_ext_result* result, std::uint32_t type, std::string_view text) const noexcept
{
    if (!GS_HOST_HAS(set_text) || m_host->set_text == nullptr) {
        return GS_EXT_UNSUPPORTED;
    }
    return m_host->set_text(result, type, text.data(), text.size());
}

std::int32_t Host::setError(gs_ext_result* result, std::string_view text) const noexcept
{
    if (!GS_HOST_HAS(set_error) || m_host->set_error == nullptr) {
        return GS_EXT_UNSUPPORTED;
    }
    return m_host->set_error(result, text.data(), text.size());
}

#undef GS_HOST_HAS

}
