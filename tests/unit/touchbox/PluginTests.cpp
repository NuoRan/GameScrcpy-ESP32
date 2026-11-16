#include "esp32/FakeBluetooth.h"
#include "esp32/TestSupport.h"

#include <gs/touchbox/Plugin.h>
#include <gs/touchbox/Texts.h>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
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

    [[nodiscard]] std::optional<std::string> result(uint64_t job)
    {
        const std::scoped_lock lock(mutex);
        const auto found = finished.find(job);
        return found != finished.end() ? std::optional<std::string>(found->second) : std::nullopt;
    }

    [[nodiscard]] std::string stored(const std::string& key)
    {
        const std::scoped_lock lock(mutex);
        const auto found = store.find(key);
        return found != store.end() ? found->second : std::string();
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

PluginOptions quietOptions(std::shared_ptr<esp32test::FakeRadio> radio = std::make_shared<esp32test::FakeRadio>())
{
    FleetHooks hooks;
    hooks.watchUsb = false;
    hooks.list = [] { return std::vector<gs::esp32::SerialPortInfo>{}; };
    return PluginOptions{.hooks = hooks, .folder = GS_PACKAGE_DIR, .radio = std::move(radio)};
}

gs::esp32::BoardKey keyOf(std::uint8_t seed)
{
    gs::esp32::BoardKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(seed + i);
    }
    return key;
}

std::shared_ptr<esp32test::FakeBleBoard> fakeBoard(const gs::esp32::BoardKey& key)
{
    esp32test::FakeBleBoard::Options options;
    options.key = key;
    return std::make_shared<esp32test::FakeBleBoard>(options);
}

bool ended(FakeHost& fake, uint64_t job)
{
    return eventually([&fake, job] { return fake.result(job).has_value() || fake.failure(job).has_value(); },
                      10000ms);
}

void start(const gs_ext_plugin& plugin, std::string_view name, uint64_t job, const std::string& args)
{
    const gs_ext_job* found = jobNamed(plugin, name);
    REQUIRE(found != nullptr);
    REQUIRE(found->start(found->context, job, args.data(), args.size()) == GS_EXT_OK);
}

constexpr std::uint64_t kNearby = 0xDEADBEEF0104ULL;
constexpr std::uint64_t kElsewhere = 0x112233445566ULL;

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
        if (name == "scan") {
            CHECK(params.empty());
            continue;
        }
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

TEST_CASE("ADR-0112: the bluetooth node lists the boards seen nearby with their signal and which of them this "
          "computer knows")
{
    FakeHost fake;
    const gs::esp32::BoardKey key = keyOf(3);
    fake.store["boards"] = gs::esp32::writeBoards({gs::esp32::KnownBoard{
        .id = "deadbeef0102", .name = "Desk C3", .key = key, .address = {}, .bluetooth = "DE:AD:BE:EF:01:04"}});
    auto radio = std::make_shared<esp32test::FakeRadio>();
    radio->add(kNearby, fakeBoard(key));
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);

    const Json listed = Json::parse(textOf(call(*plugin, "bluetooth")));
    CHECK(listed.at("state") == "ready");
    CHECK(listed.at("scanning") == false);
    REQUIRE(listed.at("boards").size() == 1);
    const Json& seen = listed.at("boards")[0];
    CHECK(seen.at("address") == "DE:AD:BE:EF:01:04");
    CHECK(seen.at("id") == "deadbeef0102");
    CHECK(seen.at("chip") == "ESP32-C3");
    CHECK(seen.at("signal") == -58);
    CHECK(seen.at("keyed") == true);
    CHECK(seen.at("known") == true);
    CHECK(seen.at("paired") == true);
    CHECK(seen.at("connected") == false);
    REQUIRE(listed.at("paired").size() == 1);
    CHECK(listed.at("paired")[0].at("address") == "DE:AD:BE:EF:01:04");
    CHECK(listed.at("paired")[0].at("name") == "Desk C3");

    radio->setState(gs::esp32::BluetoothState::Off);
    CHECK(Json::parse(textOf(call(*plugin, "bluetooth"))).at("state") == "off");
    plugin->unload(plugin->context);
}

TEST_CASE("ADR-0112: the Bluetooth jobs refuse an unknown device, a board without a key and a computer without "
          "Bluetooth")
{
    FakeHost fake;
    auto radio = std::make_shared<esp32test::FakeRadio>();
    radio->add(kNearby, fakeBoard(keyOf(1)));
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);
    const gs::touchbox::Language language = gs::touchbox::systemLanguage();

    start(*plugin, "pair", 20, R"({"device": "nonsense"})");
    REQUIRE(ended(fake, 20));
    REQUIRE(fake.failure(20).has_value());
    CHECK(fake.failure(20)->find("nonsense") != std::string::npos);

    start(*plugin, "pair", 21, R"({"device": "DE:AD:BE:EF:01:04"})");
    REQUIRE(ended(fake, 21));
    CHECK(fake.failure(21) == gs::touchbox::openFailureText(gs::esp32::OpenFailure::NoKey,
                                                            gs::esp32::LinkMode::Bluetooth, language));
    CHECK_FALSE(radio->isPaired(kNearby));

    start(*plugin, "pair", 22, R"({"device": "11:22:33:44:55:66"})");
    REQUIRE(ended(fake, 22));
    CHECK(fake.failure(22).has_value());

    start(*plugin, "unpair", 23, R"({"device": "nonsense"})");
    REQUIRE(ended(fake, 23));
    CHECK(fake.failure(23).has_value());

    radio->setState(gs::esp32::BluetoothState::Missing);
    start(*plugin, "scan", 24, "{}");
    REQUIRE(ended(fake, 24));
    CHECK(fake.failure(24) == gs::touchbox::bluetoothStateText(gs::esp32::BluetoothState::Missing, language));
    plugin->unload(plugin->context);
}

TEST_CASE("ADR-0112: pairing a board seen over Bluetooth whose key this computer has pairs it with Windows, checks "
          "the link and remembers its address; unpairing forgets the address and keeps the key")
{
    FakeHost fake;
    const gs::esp32::BoardKey key = keyOf(7);
    fake.store["boards"] =
        gs::esp32::writeBoards({gs::esp32::KnownBoard{.id = "deadbeef0102", .name = "Desk C3", .key = key}});
    auto radio = std::make_shared<esp32test::FakeRadio>();
    radio->add(kNearby, fakeBoard(key));
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);

    start(*plugin, "pair", 30, R"({"device": "de:ad:be:ef:01:04"})");
    REQUIRE(ended(fake, 30));
    INFO(fake.failure(30).value_or(std::string()));
    REQUIRE(fake.result(30).has_value());
    const Json paired = Json::parse(*fake.result(30));
    CHECK(paired.at("address") == "DE:AD:BE:EF:01:04");
    CHECK(paired.at("board") == "deadbeef0102");
    CHECK(paired.at("paired") == true);
    CHECK(radio->isPaired(kNearby));
    CHECK(radio->opened() == 1);
    std::vector<gs::esp32::KnownBoard> book = gs::esp32::parseBoards(fake.stored("boards"));
    REQUIRE(book.size() == 1);
    CHECK(book[0].bluetooth == "DE:AD:BE:EF:01:04");
    CHECK(book[0].key == key);
    CHECK(fake.hasEvent("bluetooth.changed"));

    start(*plugin, "unpair", 31, R"({"device": "deadbeef0102"})");
    REQUIRE(ended(fake, 31));
    REQUIRE(fake.result(31).has_value());
    CHECK(Json::parse(*fake.result(31)).at("unpaired") == true);
    CHECK_FALSE(radio->isPaired(kNearby));
    book = gs::esp32::parseBoards(fake.stored("boards"));
    REQUIRE(book.size() == 1);
    CHECK(book[0].bluetooth.empty());
    CHECK(book[0].key == key);

    start(*plugin, "pair", 32, R"({"device": "deadbeef0102"})");
    REQUIRE(ended(fake, 32));
    INFO(fake.failure(32).value_or(std::string()));
    REQUIRE(fake.result(32).has_value());
    CHECK(Json::parse(*fake.result(32)).at("address") == "DE:AD:BE:EF:01:04");
    CHECK(radio->isPaired(kNearby));
    CHECK(gs::esp32::parseBoards(fake.stored("boards")).at(0).bluetooth == "DE:AD:BE:EF:01:04");
    plugin->unload(plugin->context);
}

TEST_CASE("R9: pairing a board that forgot this computer drops the old Windows pairing and pairs afresh; a working "
          "pairing is kept")
{
    const gs::esp32::BoardKey key = keyOf(7);
    const std::string book = gs::esp32::writeBoards({gs::esp32::KnownBoard{
        .id = "deadbeef0102", .name = "Desk C3", .key = key, .bluetooth = "DE:AD:BE:EF:01:04"}});
    {
        FakeHost fake;
        fake.store["boards"] = book;
        auto radio = std::make_shared<esp32test::FakeRadio>();
        radio->add(kNearby, fakeBoard(key));
        radio->pairElsewhere(kNearby);
        radio->forgetOnBoard(kNearby);
        const gs_ext_host host = fake.table();
        const gs_ext_plugin* plugin = nullptr;
        REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);

        start(*plugin, "pair", 40, R"({"device": "deadbeef0102"})");
        REQUIRE(ended(fake, 40));
        INFO(fake.failure(40).value_or(std::string()));
        REQUIRE(fake.result(40).has_value());
        CHECK(Json::parse(*fake.result(40)).at("paired") == true);
        CHECK(radio->refused() == 1);
        CHECK(radio->pairings() == 1);
        CHECK(radio->opened() == 1);
        CHECK(radio->isPaired(kNearby));
        plugin->unload(plugin->context);
    }
    {
        FakeHost fake;
        fake.store["boards"] = book;
        auto radio = std::make_shared<esp32test::FakeRadio>();
        radio->add(kNearby, fakeBoard(key));
        radio->pairElsewhere(kNearby);
        const gs_ext_host host = fake.table();
        const gs_ext_plugin* plugin = nullptr;
        REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);

        start(*plugin, "pair", 41, R"({"device": "deadbeef0102"})");
        REQUIRE(ended(fake, 41));
        INFO(fake.failure(41).value_or(std::string()));
        REQUIRE(fake.result(41).has_value());
        CHECK(radio->refused() == 0);
        CHECK(radio->pairings() == 0);
        CHECK(radio->opened() == 1);
        CHECK(radio->isPaired(kNearby));
        plugin->unload(plugin->context);
    }
}

TEST_CASE("ADR-0112: unpairing refuses a Bluetooth address that is not an ESP32 touch box and leaves its pairing "
          "with Windows alone")
{
    FakeHost fake;
    auto radio = std::make_shared<esp32test::FakeRadio>();
    radio->add(kNearby, fakeBoard(keyOf(5)));
    radio->pairElsewhere(kNearby);
    radio->pairElsewhere(kElsewhere);
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin, quietOptions(radio)) == GS_EXT_OK);

    start(*plugin, "unpair", 40, R"({"device": "11:22:33:44:55:66"})");
    REQUIRE(ended(fake, 40));
    REQUIRE(fake.failure(40).has_value());
    CHECK(fake.failure(40)->find("11:22:33:44:55:66") != std::string::npos);
    CHECK(radio->isPaired(kElsewhere));

    start(*plugin, "unpair", 41, R"({"device": "DE:AD:BE:EF:01:04"})");
    REQUIRE(ended(fake, 41));
    INFO(fake.failure(41).value_or(std::string()));
    REQUIRE(fake.result(41).has_value());
    CHECK(Json::parse(*fake.result(41)).at("unpaired") == true);
    CHECK_FALSE(radio->isPaired(kNearby));
    plugin->unload(plugin->context);
}

TEST_CASE("ADR-0112: unloading the plugin while it asks a serial port stops the search and lets go of the port")
{
    struct Asking {
        std::mutex mutex;
        std::condition_variable_any changed;
        bool started = false;
    };
    const auto asking = std::make_shared<Asking>();
    FleetHooks hooks;
    hooks.watchUsb = false;
    hooks.list = [] {
        return std::vector<gs::esp32::SerialPortInfo>{gs::esp32::SerialPortInfo{.name = "COM250",
                                                                               .description = "USB-SERIAL CH340",
                                                                               .vendorId = gs::esp32::kWchVendor,
                                                                               .productId = 0x7523,
                                                                               .kind = gs::esp32::PortKind::Ch34x}};
    };
    hooks.probe = [asking](const gs::touchbox::Candidate&, const gs::esp32::Esp32Settings&, gs::esp32::LinkEvents,
                           const std::stop_token& stop, gs::esp32::BoardTrouble&) -> gs::touchbox::Probed {
        std::unique_lock lock(asking->mutex);
        asking->started = true;
        asking->changed.notify_all();
        static_cast<void>(asking->changed.wait(lock, stop, [] { return false; }));
        return gs::makeUnexpected(gs::esp32::OpenFailure::Stopped);
    };
    FakeHost fake;
    const gs_ext_host host = fake.table();
    const gs_ext_plugin* plugin = nullptr;
    REQUIRE(gs::touchbox::startPlugin(&host, &plugin,
                                      PluginOptions{.hooks = hooks,
                                                    .folder = GS_PACKAGE_DIR,
                                                    .radio = std::make_shared<esp32test::FakeRadio>()}) == GS_EXT_OK);
    {
        std::unique_lock lock(asking->mutex);
        REQUIRE(asking->changed.wait_for(lock, 10s, [&asking] { return asking->started; }));
    }
    plugin->unload(plugin->context);
    const std::scoped_lock lock(fake.mutex);
    CHECK(fake.claims == std::vector<std::string>{"serial:COM250"});
    CHECK(fake.releases == std::vector<std::string>{"serial:COM250"});
}
