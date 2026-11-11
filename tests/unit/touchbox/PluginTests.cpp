#include "esp32/TestSupport.h"

#include <gs/touchbox/Plugin.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using esp32test::eventually;
using gs::touchbox::FleetHooks;
using gs::touchbox::PluginOptions;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

uint32_t typeNamed(const std::string& name)
{
    static const std::map<std::string, uint32_t> kTypes{
        {"", GS_EXT_TYPE_NONE},         {"boolean", GS_EXT_TYPE_BOOLEAN}, {"number", GS_EXT_TYPE_NUMBER},
        {"string", GS_EXT_TYPE_STRING}, {"point", GS_EXT_TYPE_POINT},     {"region", GS_EXT_TYPE_REGION},
        {"color", GS_EXT_TYPE_COLOR},   {"duration", GS_EXT_TYPE_DURATION}, {"json", GS_EXT_TYPE_JSON}};
    const auto found = kTypes.find(name);
    return found != kTypes.end() ? found->second : 999U;
}

Json manifest()
{
    std::ifstream file(GS_EXTENSION_MANIFEST, std::ios::binary);
    REQUIRE(file.good());
    return Json::parse(file);
}

std::vector<std::string> namesOf(const Json& list)
{
    std::vector<std::string> names;
    for (const Json& entry : list) {
        names.push_back(entry.at("name").get<std::string>());
    }
    return names;
}

std::vector<const gs_ext_contribution*> contributionsOf(const gs_ext_plugin& plugin, std::string_view kind)
{
    std::vector<const gs_ext_contribution*> found;
    for (uint32_t index = 0; index < plugin.contribution_count; ++index) {
        if (std::string_view(plugin.contributions[index].kind) == kind) {
            found.push_back(&plugin.contributions[index]);
        }
    }
    return found;
}

std::vector<std::string> namesOf(const std::vector<const gs_ext_contribution*>& contributions)
{
    std::vector<std::string> names;
    for (const gs_ext_contribution* contribution : contributions) {
        names.emplace_back(contribution->name);
    }
    return names;
}

struct FakeHost {
    std::mutex mutex;
    std::map<std::string, std::string> store;
    std::map<std::string, std::string> settings{{"port", "\"auto\""}, {"link", "\"serial\""}, {"address", "\"\""}};
    std::vector<std::pair<std::string, std::string>> events;
    std::map<uint64_t, std::string> failed;
    std::map<uint64_t, std::string> finished;
    std::vector<std::string> claims;
    std::vector<std::string> releases;
    int devicesChanged = 0;
    std::string box;

    static FakeHost& of(void* context) { return *static_cast<FakeHost*>(context); }

    static void log(void*, uint32_t, const char*, size_t) {}

    static uint64_t now(void*) { return 0; }

    static int32_t storeGet(void* context, const char* key, size_t keySize, gs_ext_result* value)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        value->value = gs_ext_value{};
        const auto found = host.store.find(std::string(key, keySize));
        if (found == host.store.end()) {
            value->value.type = GS_EXT_TYPE_NONE;
            return GS_EXT_OK;
        }
        thread_local std::string held;
        held = found->second;
        value->value.type = GS_EXT_TYPE_JSON;
        value->value.text = held.data();
        value->value.size = held.size();
        return GS_EXT_OK;
    }

    static int32_t storeSet(void* context, const char* key, size_t keySize, const char* json, size_t jsonSize)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.store[std::string(key, keySize)] = std::string(json, jsonSize);
        return GS_EXT_OK;
    }

    static int32_t allowed(void*, const char*) { return 1; }

    static int32_t notify(void*, const char*, size_t, const char*, size_t) { return GS_EXT_OK; }

    static int32_t setText(gs_ext_result* result, uint32_t type, const char* text, size_t size)
    {
        thread_local std::string held;
        held.assign(text != nullptr ? text : "", size);
        result->value = gs_ext_value{};
        result->value.type = type;
        result->value.text = held.data();
        result->value.size = held.size();
        return GS_EXT_OK;
    }

    static int32_t setError(gs_ext_result*, const char*, size_t) { return GS_EXT_OK; }

    static int32_t supports(void*, const char* feature)
    {
        const std::string_view name(feature);
        return name == "jobs" || name == "events" || name == "settings" || name == "claims" ? 1 : 0;
    }

    static int32_t progress(void*, uint64_t, double, const char*, size_t) { return GS_EXT_OK; }

    static int32_t jobLog(void*, uint64_t, uint32_t, const char*, size_t) { return GS_EXT_OK; }

    static int32_t finish(void* context, uint64_t job, const char* json, size_t size)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.finished[job] = std::string(json, size);
        return GS_EXT_OK;
    }

    static int32_t fail(void* context, uint64_t job, const char* text, size_t size)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.failed[job] = std::string(text, size);
        return GS_EXT_OK;
    }

    static int32_t emit(void* context, const char* topic, size_t topicSize, const char* json, size_t jsonSize)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.events.emplace_back(std::string(topic, topicSize), std::string(json, jsonSize));
        return GS_EXT_OK;
    }

    static int32_t settingGet(void* context, const char* key, size_t keySize, const char*, size_t,
                              gs_ext_result* value)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        const auto found = host.settings.find(std::string(key, keySize));
        if (found == host.settings.end()) {
            return GS_EXT_BAD_ARGUMENT;
        }
        thread_local std::string held;
        held = found->second;
        value->value = gs_ext_value{};
        value->value.type = GS_EXT_TYPE_JSON;
        value->value.text = held.data();
        value->value.size = held.size();
        return GS_EXT_OK;
    }

    static int32_t changed(void* context, const char*, size_t)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        ++host.devicesChanged;
        return GS_EXT_OK;
    }

    static int32_t claim(void* context, const char* resource, size_t size, uint32_t)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.claims.emplace_back(resource, size);
        return GS_EXT_OK;
    }

    static int32_t release(void* context, const char* resource, size_t size)
    {
        FakeHost& host = of(context);
        const std::scoped_lock lock(host.mutex);
        host.releases.emplace_back(resource, size);
        return GS_EXT_OK;
    }

    [[nodiscard]] gs_ext_host table()
    {
        gs_ext_host made{};
        made.size = sizeof(gs_ext_host);
        made.abi_version = GS_EXT_ABI_VERSION;
        made.context = this;
        made.log = &FakeHost::log;
        made.now_us = &FakeHost::now;
        made.store_get = &FakeHost::storeGet;
        made.store_set = &FakeHost::storeSet;
        made.allowed = &FakeHost::allowed;
        made.notify = &FakeHost::notify;
        made.set_text = &FakeHost::setText;
        made.set_error = &FakeHost::setError;
        made.supports = &FakeHost::supports;
        made.job_progress = &FakeHost::progress;
        made.job_log = &FakeHost::jobLog;
        made.job_finish = &FakeHost::finish;
        made.job_fail = &FakeHost::fail;
        made.emit_event = &FakeHost::emit;
        made.setting_get = &FakeHost::settingGet;
        made.devices_changed = &FakeHost::changed;
        made.claim = &FakeHost::claim;
        made.release = &FakeHost::release;
        return made;
    }

    [[nodiscard]] bool hasEvent(std::string_view topic)
    {
        const std::scoped_lock lock(mutex);
        return std::ranges::any_of(events, [topic](const auto& event) { return event.first == topic; });
    }

    [[nodiscard]] std::optional<std::string> failure(uint64_t job)
    {
        const std::scoped_lock lock(mutex);
        const auto found = failed.find(job);
        return found != failed.end() ? std::optional<std::string>(found->second) : std::nullopt;
    }
};

const gs_ext_node* nodeNamed(const gs_ext_plugin& plugin, std::string_view name)
{
    for (const gs_ext_contribution* contribution : contributionsOf(plugin, GS_EXT_KIND_NODE)) {
        if (std::string_view(contribution->name) == name) {
            return static_cast<const gs_ext_node*>(contribution->data);
        }
    }
    return nullptr;
}

const gs_ext_job* jobNamed(const gs_ext_plugin& plugin, std::string_view name)
{
    for (const gs_ext_contribution* contribution : contributionsOf(plugin, GS_EXT_KIND_JOB)) {
        if (std::string_view(contribution->name) == name) {
            return static_cast<const gs_ext_job*>(contribution->data);
        }
    }
    return nullptr;
}

std::string textOf(const gs_ext_result& result)
{
    return result.value.text != nullptr ? std::string(result.value.text, result.value.size) : std::string();
}

gs_ext_result call(const gs_ext_plugin& plugin, std::string_view name, const std::vector<gs_ext_value>& args = {})
{
    const gs_ext_node* node = nodeNamed(plugin, name);
    REQUIRE(node != nullptr);
    gs_ext_result result{};
    CHECK(node->call(node->context, args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()), &result) ==
          GS_EXT_OK);
    return result;
}

gs_ext_value textValue(const char* text)
{
    gs_ext_value value{};
    value.type = GS_EXT_TYPE_STRING;
    value.text = text;
    value.size = std::strlen(text);
    return value;
}

PluginOptions quietOptions()
{
    FleetHooks hooks;
    hooks.watchUsb = false;
    hooks.list = [] { return std::vector<gs::esp32::SerialPortInfo>{}; };
    return PluginOptions{.hooks = hooks, .folder = GS_PACKAGE_DIR};
}

}

TEST_CASE("ADR-0112: the DLL offers exactly the touch method, provider, jobs and nodes the manifest declares")
{
    const gs_ext_plugin& plugin = gs::touchbox::pluginDescription();
    CHECK(plugin.size == sizeof(gs_ext_plugin));
    CHECK(plugin.abi_version == GS_EXT_ABI_VERSION);
    CHECK(plugin.unload != nullptr);
    CHECK(plugin.settings_changed != nullptr);
    CHECK(plugin.claim_wanted != nullptr);
    for (uint32_t index = 0; index < plugin.contribution_count; ++index) {
        CHECK(plugin.contributions[index].size == sizeof(gs_ext_contribution));
        CHECK(plugin.contributions[index].data != nullptr);
    }

    const Json native = manifest().at("contributes").at("native");
    CHECK(native.at("dll") == "esp32.dll");
    CHECK(namesOf(contributionsOf(plugin, GS_EXT_KIND_TOUCH_METHOD)) == namesOf(native.at("touchMethods")));
    CHECK(namesOf(contributionsOf(plugin, GS_EXT_KIND_DEVICE_PROVIDER)) == namesOf(native.at("deviceProviders")));
    CHECK(namesOf(contributionsOf(plugin, GS_EXT_KIND_JOB)) == namesOf(native.at("jobs")));
    CHECK(namesOf(contributionsOf(plugin, GS_EXT_KIND_NODE)) == namesOf(native.at("nodes")));

    for (const Json& declared : native.at("nodes")) {
        const std::string name = declared.at("name").get<std::string>();
        INFO(name);
        const gs_ext_node* node = nodeNamed(plugin, name);
        REQUIRE(node != nullptr);
        CHECK(node->size == sizeof(gs_ext_node));
        CHECK(std::string_view(node->name) == name);
        CHECK(node->returns == typeNamed(declared.value("returns", std::string())));
        const Json params = declared.value("params", Json::array());
        REQUIRE(node->param_count == params.size());
        for (std::size_t index = 0; index < params.size(); ++index) {
            CHECK(node->params[index] == typeNamed(params[index].at("type").get<std::string>()));
        }
    }
    for (const Json& declared : native.at("jobs")) {
        const std::string name = declared.at("name").get<std::string>();
        INFO(name);
        const gs_ext_job* job = jobNamed(plugin, name);
        REQUIRE(job != nullptr);
        CHECK(std::string_view(job->name) == name);
        CHECK(job->start != nullptr);
        CHECK(job->cancel != nullptr);
        const std::vector<std::string> params = namesOf(declared.at("params"));
        REQUIRE_FALSE(params.empty());
        CHECK(params.front() == "device");
    }
    const auto* method =
        static_cast<const gs_ext_touch_method*>(contributionsOf(plugin, GS_EXT_KIND_TOUCH_METHOD).front()->data);
    CHECK(method->max_contacts == 10);
    CHECK(method->needs_session == 0);
    CHECK(method->mouse != nullptr);
    CHECK(method->gamepad != nullptr);
}

TEST_CASE("ADR-0112: the manifest names the panels, the help in two languages and the settings each phone can have")
{
    const Json root = manifest();
    CHECK(root.at("id") == "community.esp32");
    CHECK(root.at("authors") == Json::array({"GameScrcpy 社区"}));
    CHECK(root.at("license") == "Apache-2.0");
    const Json& contributes = root.at("contributes");
    REQUIRE(contributes.at("panels").size() == 2);
    CHECK(contributes.at("panels")[0].at("where") == "extension");
    CHECK(contributes.at("panels")[1].at("where") == "device");
    CHECK(contributes.at("panels")[1].at("name") == "board");
    CHECK(contributes.at("scripts")[0].at("run") == "app");
    CHECK(contributes.at("help")[0].at("locales").at("zh_CN") == "help/esp32.zh_CN.md");
    for (const Json& setting : root.at("settings")) {
        CHECK(setting.at("scope") == "device");
    }
    std::vector<std::string> permissions;
    for (const Json& permission : root.at("permissions")) {
        permissions.push_back(permission.at("name").get<std::string>());
    }
    CHECK(permissions == std::vector<std::string>{"hardware", "devices.provide", "network"});
}

TEST_CASE("ADR-0112: the plugin answers its nodes, lists no device without boards and refuses what it does not know")
{
    FakeHost fake;
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions()) == GS_EXT_OK);
    REQUIRE(plugin != nullptr);

    CHECK(Json::parse(textOf(call(*plugin, "boards"))) == Json::array());
    CHECK(Json::parse(textOf(call(*plugin, "ports"))) == Json::array());
    const Json firmware = Json::parse(textOf(call(*plugin, "firmware")));
    REQUIRE(firmware.is_object());
    CHECK_FALSE(firmware.at("version").get<std::string>().empty());
    CHECK(firmware.at("targets").size() == 6);
    const std::string language = textOf(call(*plugin, "language"));
    CHECK((language == "en" || language == "zh_CN"));
    CHECK(call(*plugin, "rescan").value.flag == 1);
    CHECK(call(*plugin, "tap", {textValue("serial-com7")}).value.flag == 0);
    CHECK(call(*plugin, "disconnect", {textValue("serial-com7")}).value.flag == 0);

    const gs_ext_node* tap = nodeNamed(*plugin, "tap");
    gs_ext_result ignored{};
    CHECK(tap->call(tap->context, nullptr, 0, &ignored) == GS_EXT_BAD_ARGUMENT);

    const auto* provider = static_cast<const gs_ext_device_provider*>(
        contributionsOf(*plugin, GS_EXT_KIND_DEVICE_PROVIDER).front()->data);
    gs_ext_result listed{};
    REQUIRE(provider->list(provider->context, &listed) == GS_EXT_OK);
    CHECK(Json::parse(textOf(listed)) == Json::array());
    CHECK(provider->act(provider->context, "serial-com7", 11, "tap", 3) == GS_EXT_BAD_ARGUMENT);

    CHECK(plugin->claim_wanted(plugin->context, "serial:COM9", 11, "Your logic", 10) == GS_EXT_OK);
    plugin->settings_changed(plugin->context, "[\"port\"]", 8);
    plugin->unload(plugin->context);
}

TEST_CASE("ADR-0112: a job for a device that is neither a board nor a serial port fails and says so in an event")
{
    FakeHost fake;
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions()) == GS_EXT_OK);

    const gs_ext_job* diagnose = jobNamed(*plugin, "diagnose");
    REQUIRE(diagnose != nullptr);
    const std::string args = R"({"device": "nonsense"})";
    REQUIRE(diagnose->start(diagnose->context, 7, args.data(), args.size()) == GS_EXT_OK);
    REQUIRE(eventually([&fake] { return fake.failure(7).has_value(); }));
    CHECK_FALSE(fake.failure(7)->empty());
    CHECK(eventually([&fake] { return fake.hasEvent("board.diagnosed"); }));

    const gs_ext_job* flash = jobNamed(*plugin, "flash");
    REQUIRE(flash != nullptr);
    REQUIRE(flash->start(flash->context, 8, args.data(), args.size()) == GS_EXT_OK);
    REQUIRE(eventually([&fake] { return fake.failure(8).has_value(); }));
    CHECK(eventually([&fake] { return fake.hasEvent("flash.done"); }));

    const gs_ext_job* wifi = jobNamed(*plugin, "wifi");
    const std::string wifiArgs = R"({"device": "serial-com7", "ssid": "Home", "password": "secret"})";
    REQUIRE(wifi->start(wifi->context, 9, wifiArgs.data(), wifiArgs.size()) == GS_EXT_OK);
    REQUIRE(eventually([&fake] { return fake.failure(9).has_value(); }));

    const gs_ext_job* rename = jobNamed(*plugin, "rename");
    const std::string renameArgs = R"({"device": "serial-com7", "name": "Desk"})";
    REQUIRE(rename->start(rename->context, 10, renameArgs.data(), renameArgs.size()) == GS_EXT_OK);
    REQUIRE(eventually([&fake] { return fake.failure(10).has_value(); }));

    CHECK(diagnose->start(diagnose->context, 11, "{", 1) == GS_EXT_BAD_ARGUMENT);
    plugin->unload(plugin->context);
}

TEST_CASE("ADR-0112: the plugin refuses a host of another interface version")
{
    FakeHost fake;
    gs_ext_host host = fake.table();
    host.abi_version = 1;
    const gs_ext_plugin* plugin = nullptr;
    CHECK(gs::touchbox::startPlugin(&host, &plugin, quietOptions()) == GS_EXT_UNSUPPORTED);
    CHECK(plugin == nullptr);
}
