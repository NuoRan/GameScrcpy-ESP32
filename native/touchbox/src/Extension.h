#pragma once

#include "gs/touchbox/Fleet.h"
#include "gs/touchbox/Host.h"
#include "gs/touchbox/Plugin.h"
#include "gs/touchbox/Texts.h"

#include <gs/esp32/Boards.h>
#include <gs/esp32/TouchMethod.h>
#include <gs/espflash/Firmware.h>
#include <gs/support/Log.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gs::touchbox {

enum class JobKind : std::uint8_t { Flash, Wifi, Rename, Diagnose };

class Extension final
{
public:
    Extension(const gs_ext_host* host, PluginOptions options);
    ~Extension();
    Extension(const Extension&) = delete;
    Extension& operator=(const Extension&) = delete;

    [[nodiscard]] std::int32_t openTouch(const gs_ext_touch_open* info, void** session);
    void closeTouch(void* session);
    [[nodiscard]] std::int32_t touchFrame(void* session, const gs_ext_touch_frame* frame);
    [[nodiscard]] std::int32_t touchKey(void* session, const gs_ext_key* key);
    [[nodiscard]] std::int32_t touchText(void* session, std::string_view typed);
    [[nodiscard]] std::int32_t touchMouse(void* session, const gs_ext_mouse* mouse);
    [[nodiscard]] std::int32_t touchGamepad(void* session, const gs_ext_gamepad* pad);

    [[nodiscard]] std::int32_t listDevices(gs_ext_result* out);
    [[nodiscard]] std::int32_t act(std::string_view device, std::string_view action);

    [[nodiscard]] std::int32_t boardsNode(gs_ext_result* result);
    [[nodiscard]] std::int32_t portsNode(gs_ext_result* result);
    [[nodiscard]] std::int32_t firmwareNode(gs_ext_result* result);
    [[nodiscard]] std::int32_t languageNode(gs_ext_result* result);
    [[nodiscard]] std::int32_t problemNode(gs_ext_result* result);
    [[nodiscard]] std::int32_t tapNode(std::string_view device, gs_ext_result* result);
    [[nodiscard]] std::int32_t disconnectNode(std::string_view device, gs_ext_result* result);
    [[nodiscard]] std::int32_t rescanNode(gs_ext_result* result);

    [[nodiscard]] std::int32_t startJob(JobKind kind, std::uint64_t job, std::string_view args);
    void cancelJob(std::uint64_t job);

    void settingsChanged();
    [[nodiscard]] std::int32_t claimWanted(std::string_view resource, std::string_view by);

private:
    struct Session {
        std::string owner;
        std::string resource;
        esp32::Esp32TouchMethod method;
    };

    struct Running {
        std::jthread thread;
        std::atomic<bool> done{false};
    };

    class Sink final : public log::Sink
    {
    public:
        explicit Sink(const Host& host) noexcept
            : m_host(host)
        {
        }

        void write(const log::Record& record) noexcept override;

    private:
        const Host& m_host;
    };

    [[nodiscard]] std::optional<std::string> textSetting(std::string_view key) const;
    [[nodiscard]] esp32::Esp32Settings globalSettings() const;
    [[nodiscard]] esp32::Esp32Settings settingsFor(std::string_view options) const;
    [[nodiscard]] std::vector<esp32::KnownBoard> knownBoards() const;
    [[nodiscard]] std::vector<esp32::KnownBoard> loadKnown() const;
    void rememberBoard(const esp32::KnownBoard& board);
    void forgetBoard(std::string_view id);
    void saveKnown(const std::vector<esp32::KnownBoard>& known);
    void fleetChanged();
    void emit(std::string_view topic, std::string_view json) const;
    [[nodiscard]] std::int32_t jsonResult(gs_ext_result* result, std::string_view json) const;
    [[nodiscard]] static std::int32_t flagResult(gs_ext_result* result, bool value) noexcept;
    void runJob(JobKind kind, std::uint64_t job, const std::string& args, const std::stop_token& stop);
    void runFlash(std::uint64_t job, const std::string& device, const std::stop_token& stop);
    void runWifi(std::uint64_t job, const std::string& device, const std::string& ssid, const std::string& password);
    void runRename(std::uint64_t job, const std::string& device, const std::string& name);
    void runDiagnose(std::uint64_t job, const std::string& device, const std::stop_token& stop);
    void reapJobs();

    const Host m_host;
    const Language m_language;
    Sink m_sink;
    std::filesystem::path m_firmwareFolder;
    std::optional<espflash::FirmwareBundle> m_bundle;
    mutable std::mutex m_mutex;
    std::vector<esp32::KnownBoard> m_known;
    std::vector<Board> m_shown;
    std::vector<esp32::SerialPortInfo> m_shownPorts;
    std::atomic<std::uint64_t> m_sessions{0};
    std::mutex m_jobsMutex;
    std::map<std::uint64_t, std::unique_ptr<Running>> m_jobs;
    bool m_stopping = false;
    std::unique_ptr<Fleet> m_fleet;
};

}
