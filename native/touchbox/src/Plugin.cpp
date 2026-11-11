#include "gs/touchbox/Plugin.h"

#include "Extension.h"

#include <iterator>
#include <memory>
#include <string_view>
#include <utility>

namespace gs::touchbox {

namespace {

std::unique_ptr<Extension> g_extension;

template <typename Work>
std::int32_t guarded(Work&& work) noexcept
{
    if (g_extension == nullptr) {
        return GS_EXT_UNSUPPORTED;
    }
    try {
        return std::forward<Work>(work)(*g_extension);
    } catch (...) {
        return GS_EXT_FAILED;
    }
}

template <typename Work>
void quietly(Work&& work) noexcept
{
    if (g_extension == nullptr) {
        return;
    }
    try {
        std::forward<Work>(work)(*g_extension);
    } catch (...) {
    }
}

std::string_view textOf(const char* text, size_t size) noexcept
{
    return text == nullptr || size == 0 ? std::string_view() : std::string_view(text, size);
}

std::optional<std::string_view> deviceArgument(const gs_ext_value* args, uint32_t count) noexcept
{
    if (args == nullptr || count != 1 || args[0].type != GS_EXT_TYPE_STRING ||
        (args[0].text == nullptr && args[0].size > 0)) {
        return std::nullopt;
    }
    return textOf(args[0].text, args[0].size);
}

int32_t openTouch(void*, const gs_ext_touch_open* info, void** session)
{
    return guarded([info, session](Extension& extension) { return extension.openTouch(info, session); });
}

void closeTouch(void*, void* session)
{
    quietly([session](Extension& extension) { extension.closeTouch(session); });
}

int32_t frameTouch(void*, void* session, const gs_ext_touch_frame* frame)
{
    return guarded([session, frame](Extension& extension) { return extension.touchFrame(session, frame); });
}

int32_t keyTouch(void*, void* session, const gs_ext_key* key)
{
    return guarded([session, key](Extension& extension) { return extension.touchKey(session, key); });
}

int32_t textTouch(void*, void* session, const char* text, size_t size)
{
    if (text == nullptr || size == 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([session, typed = std::string_view(text, size)](Extension& extension) {
        return extension.touchText(session, typed);
    });
}

int32_t mouseTouch(void*, void* session, const gs_ext_mouse* mouse)
{
    return guarded([session, mouse](Extension& extension) { return extension.touchMouse(session, mouse); });
}

int32_t gamepadTouch(void*, void* session, const gs_ext_gamepad* pad)
{
    return guarded([session, pad](Extension& extension) { return extension.touchGamepad(session, pad); });
}

int32_t listBoards(void*, gs_ext_result* out)
{
    return guarded([out](Extension& extension) { return extension.listDevices(out); });
}

int32_t actOnBoard(void*, const char* device, size_t deviceSize, const char* action, size_t actionSize)
{
    return guarded([deviceId = textOf(device, deviceSize), actionId = textOf(action, actionSize)](Extension& extension) {
        return extension.act(deviceId, actionId);
    });
}

int32_t callBoards(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.boardsNode(result); });
}

int32_t callPorts(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.portsNode(result); });
}

int32_t callFirmware(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.firmwareNode(result); });
}

int32_t callLanguage(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.languageNode(result); });
}

int32_t callProblem(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.problemNode(result); });
}

int32_t callTap(void*, const gs_ext_value* args, uint32_t count, gs_ext_result* result)
{
    const std::optional<std::string_view> device = deviceArgument(args, count);
    if (!device) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([&device, result](Extension& extension) { return extension.tapNode(*device, result); });
}

int32_t callDisconnect(void*, const gs_ext_value* args, uint32_t count, gs_ext_result* result)
{
    const std::optional<std::string_view> device = deviceArgument(args, count);
    if (!device) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([&device, result](Extension& extension) { return extension.disconnectNode(*device, result); });
}

int32_t callRescan(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.rescanNode(result); });
}

int32_t callBluetooth(void*, const gs_ext_value*, uint32_t count, gs_ext_result* result)
{
    if (count != 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([result](Extension& extension) { return extension.bluetoothNode(result); });
}

int32_t startWith(JobKind kind, uint64_t job, const char* args, size_t size)
{
    if (args == nullptr && size > 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([kind, job, text = textOf(args, size)](Extension& extension) {
        return extension.startJob(kind, job, text);
    });
}

int32_t startFlash(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Flash, job, args, size);
}

int32_t startWifi(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Wifi, job, args, size);
}

int32_t startRename(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Rename, job, args, size);
}

int32_t startDiagnose(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Diagnose, job, args, size);
}

int32_t startScan(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Scan, job, args, size);
}

int32_t startPair(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Pair, job, args, size);
}

int32_t startUnpair(void*, uint64_t job, const char* args, size_t size)
{
    return startWith(JobKind::Unpair, job, args, size);
}

void cancelRunning(void*, uint64_t job)
{
    quietly([job](Extension& extension) { extension.cancelJob(job); });
}

void unloadPlugin(void*)
{
    try {
        g_extension.reset();
    } catch (...) {
    }
}

void settingsChanged(void*, const char*, size_t)
{
    quietly([](Extension& extension) { extension.settingsChanged(); });
}

int32_t claimWanted(void*, const char* resource, size_t size, const char* by, size_t bySize)
{
    if (resource == nullptr || size == 0) {
        return GS_EXT_BAD_ARGUMENT;
    }
    return guarded([wanted = std::string_view(resource, size), who = textOf(by, bySize)](Extension& extension) {
        return extension.claimWanted(wanted, who);
    });
}

const uint32_t kDeviceParam[] = {GS_EXT_TYPE_STRING};

const gs_ext_touch_method kTouchMethod = {
    .size = sizeof(gs_ext_touch_method),
    .name = "esp32",
    .max_contacts = static_cast<uint32_t>(esp32::kMaxContacts),
    .can_cancel = 0,
    .needs_session = 0,
    .context = nullptr,
    .open = &openTouch,
    .close = &closeTouch,
    .frame = &frameTouch,
    .key = &keyTouch,
    .text = &textTouch,
    .mouse = &mouseTouch,
    .gamepad = &gamepadTouch,
};

const gs_ext_device_provider kProvider = {
    .size = sizeof(gs_ext_device_provider),
    .name = "boards",
    .context = nullptr,
    .list = &listBoards,
    .act = &actOnBoard,
};

const gs_ext_job kJobs[] = {
    {.size = sizeof(gs_ext_job), .name = "flash", .context = nullptr, .start = &startFlash, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "wifi", .context = nullptr, .start = &startWifi, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "rename", .context = nullptr, .start = &startRename, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "diagnose", .context = nullptr, .start = &startDiagnose, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "scan", .context = nullptr, .start = &startScan, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "pair", .context = nullptr, .start = &startPair, .cancel = &cancelRunning},
    {.size = sizeof(gs_ext_job), .name = "unpair", .context = nullptr, .start = &startUnpair, .cancel = &cancelRunning},
};

const gs_ext_node kNodes[] = {
    {.size = sizeof(gs_ext_node), .name = "boards", .params = nullptr, .param_count = 0, .returns = GS_EXT_TYPE_JSON,
     .context = nullptr, .call = &callBoards},
    {.size = sizeof(gs_ext_node), .name = "ports", .params = nullptr, .param_count = 0, .returns = GS_EXT_TYPE_JSON,
     .context = nullptr, .call = &callPorts},
    {.size = sizeof(gs_ext_node), .name = "firmware", .params = nullptr, .param_count = 0, .returns = GS_EXT_TYPE_JSON,
     .context = nullptr, .call = &callFirmware},
    {.size = sizeof(gs_ext_node), .name = "language", .params = nullptr, .param_count = 0,
     .returns = GS_EXT_TYPE_STRING, .context = nullptr, .call = &callLanguage},
    {.size = sizeof(gs_ext_node), .name = "problem", .params = nullptr, .param_count = 0,
     .returns = GS_EXT_TYPE_STRING, .context = nullptr, .call = &callProblem},
    {.size = sizeof(gs_ext_node), .name = "tap", .params = kDeviceParam, .param_count = 1,
     .returns = GS_EXT_TYPE_BOOLEAN, .context = nullptr, .call = &callTap},
    {.size = sizeof(gs_ext_node), .name = "disconnect", .params = kDeviceParam, .param_count = 1,
     .returns = GS_EXT_TYPE_BOOLEAN, .context = nullptr, .call = &callDisconnect},
    {.size = sizeof(gs_ext_node), .name = "rescan", .params = nullptr, .param_count = 0,
     .returns = GS_EXT_TYPE_BOOLEAN, .context = nullptr, .call = &callRescan},
    {.size = sizeof(gs_ext_node), .name = "bluetooth", .params = nullptr, .param_count = 0,
     .returns = GS_EXT_TYPE_JSON, .context = nullptr, .call = &callBluetooth},
};

const gs_ext_contribution kContributions[] = {
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_TOUCH_METHOD, .name = "esp32", .data = &kTouchMethod},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_DEVICE_PROVIDER, .name = "boards", .data = &kProvider},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "flash", .data = &kJobs[0]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "wifi", .data = &kJobs[1]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "rename", .data = &kJobs[2]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "diagnose", .data = &kJobs[3]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "scan", .data = &kJobs[4]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "pair", .data = &kJobs[5]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_JOB, .name = "unpair", .data = &kJobs[6]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "boards", .data = &kNodes[0]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "ports", .data = &kNodes[1]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "firmware", .data = &kNodes[2]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "language", .data = &kNodes[3]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "problem", .data = &kNodes[4]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "tap", .data = &kNodes[5]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "disconnect", .data = &kNodes[6]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "rescan", .data = &kNodes[7]},
    {.size = sizeof(gs_ext_contribution), .kind = GS_EXT_KIND_NODE, .name = "bluetooth", .data = &kNodes[8]},
};

const gs_ext_plugin kPlugin = {
    .size = sizeof(gs_ext_plugin),
    .abi_version = GS_EXT_ABI_VERSION,
    .context = nullptr,
    .contribution_count = static_cast<uint32_t>(std::size(kContributions)),
    .contributions = kContributions,
    .unload = &unloadPlugin,
    .settings_changed = &settingsChanged,
    .claim_wanted = &claimWanted,
};

}

std::int32_t startPlugin(const gs_ext_host* host, const gs_ext_plugin** plugin, PluginOptions options)
{
    if (host == nullptr || plugin == nullptr || host->abi_version != GS_EXT_ABI_VERSION) {
        return GS_EXT_UNSUPPORTED;
    }
    try {
        g_extension.reset();
        g_extension = std::make_unique<Extension>(host, std::move(options));
    } catch (...) {
        g_extension.reset();
        return GS_EXT_FAILED;
    }
    *plugin = &kPlugin;
    return GS_EXT_OK;
}

const gs_ext_plugin& pluginDescription() noexcept
{
    return kPlugin;
}

}
