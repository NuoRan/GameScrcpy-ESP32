#include "gs/esp32/Bluetooth.h"

#include <gs/support/Log.h>
#include <gs/support/Thread.h>

#include <objbase.h>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace gs::esp32 {

namespace {

namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace advertising = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace radios = winrt::Windows::Devices::Radios;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;
using winrt::Windows::Storage::Streams::IBuffer;

constexpr std::string_view kCategory = "esp32";
constexpr std::uint8_t kServiceData128 = 0x21;
constexpr std::size_t kSmallestChunk = 20;
constexpr std::size_t kAttHeader = 3;
constexpr std::size_t kMostSeen = 256;
constexpr std::chrono::seconds kStateAge{5};

winrt::guid guidOf(const BluetoothUuid& bytes) noexcept
{
    const auto data1 = static_cast<std::uint32_t>(bytes[15]) << 24U | static_cast<std::uint32_t>(bytes[14]) << 16U |
                       static_cast<std::uint32_t>(bytes[13]) << 8U | static_cast<std::uint32_t>(bytes[12]);
    const auto data2 = static_cast<std::uint16_t>(static_cast<unsigned>(bytes[11]) << 8U | bytes[10]);
    const auto data3 = static_cast<std::uint16_t>(static_cast<unsigned>(bytes[9]) << 8U | bytes[8]);
    const std::array<std::uint8_t, 8> data4{bytes[7], bytes[6], bytes[5], bytes[4],
                                            bytes[3], bytes[2], bytes[1], bytes[0]};
    return winrt::guid(data1, data2, data3, data4);
}

std::vector<std::uint8_t> bytesOf(const IBuffer& buffer)
{
    if (!buffer) {
        return {};
    }
    winrt::com_array<std::uint8_t> array;
    CryptographicBuffer::CopyToByteArray(buffer, array);
    return std::vector<std::uint8_t>(array.begin(), array.end());
}

std::string messageOf(const winrt::hresult_error& error)
{
    const std::string message = winrt::to_string(error.message());
    return message.empty() ? std::string("Bluetooth failed") : message;
}

BleFailure failureOf(gatt::GattCommunicationStatus status) noexcept
{
    switch (status) {
    case gatt::GattCommunicationStatus::Success:
        return BleFailure::None;
    case gatt::GattCommunicationStatus::Unreachable:
        return BleFailure::Unreachable;
    case gatt::GattCommunicationStatus::ProtocolError:
        return BleFailure::NotPaired;
    case gatt::GattCommunicationStatus::AccessDenied:
        return BleFailure::Refused;
    }
    return BleFailure::Unreachable;
}

std::size_t chunkOf(std::uint16_t pdu) noexcept
{
    return std::max(static_cast<std::size_t>(pdu) > kAttHeader ? static_cast<std::size_t>(pdu) - kAttHeader : 0U,
                    kSmallestChunk);
}

class Worker
{
public:
    Worker()
        : m_thread([this](const std::stop_token& stop) { run(stop); })
    {
    }

    ~Worker() = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> work)
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_work.push_back(std::move(work));
        }
        m_changed.notify_all();
    }

    template <typename Result>
    Result call(const std::function<Result()>& work)
    {
        if (std::this_thread::get_id() == m_thread.get_id()) {
            return work();
        }
        std::promise<Result> promise;
        std::future<Result> future = promise.get_future();
        post([&promise, &work] {
            try {
                promise.set_value(work());
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
        });
        return future.get();
    }

private:
    void run(const std::stop_token& stop)
    {
        platform::setCurrentThreadName("ESP32 Bluetooth worker");
        const HRESULT joined = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> next;
            {
                std::unique_lock lock(m_mutex);
                static_cast<void>(m_changed.wait(lock, stop, [this] { return !m_work.empty(); }));
                if (m_work.empty()) {
                    break;
                }
                next = std::move(m_work.front());
                m_work.pop_front();
            }
            next();
        }
        if (SUCCEEDED(joined)) {
            CoUninitialize();
        }
    }

    std::mutex m_mutex;
    std::condition_variable_any m_changed;
    std::deque<std::function<void()>> m_work;
    std::jthread m_thread;
};

struct Link {
    bluetooth::BluetoothLEDevice device{nullptr};
    gatt::GattSession session{nullptr};
    gatt::GattDeviceService service{nullptr};
    gatt::GattCharacteristic receive{nullptr};
    gatt::GattCharacteristic send{nullptr};
    bluetooth::BluetoothLEPreferredConnectionParametersRequest request{nullptr};
    winrt::event_token valueToken{};
    winrt::event_token statusToken{};
    winrt::event_token sizeToken{};
    std::atomic<std::size_t> chunk{kSmallestChunk};
    std::atomic<bool> ended{false};
    BleChannelEvents events;
};

void closeLink(Link& link)
{
    link.ended.store(true, std::memory_order_release);
    try {
        if (link.send && link.valueToken.value != 0) {
            link.send.ValueChanged(link.valueToken);
        }
        if (link.device && link.statusToken.value != 0) {
            link.device.ConnectionStatusChanged(link.statusToken);
        }
        if (link.session && link.sizeToken.value != 0) {
            link.session.MaxPduSizeChanged(link.sizeToken);
        }
        if (link.request) {
            link.request.Close();
        }
        if (link.session) {
            link.session.MaintainConnection(false);
            link.session.Close();
        }
        if (link.device) {
            link.device.Close();
        }
    } catch (const winrt::hresult_error& error) {
        log::info(kCategory, "closing the Bluetooth link: {}", messageOf(error));
    }
    link.request = nullptr;
    link.receive = nullptr;
    link.send = nullptr;
    link.service = nullptr;
    link.session = nullptr;
    link.device = nullptr;
}

BleFailure connectLink(Link& link, std::uint64_t address, const std::weak_ptr<Link>& weak)
{
    link.device = bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
    if (!link.device) {
        return BleFailure::NotFound;
    }
    link.session = gatt::GattSession::FromDeviceIdAsync(link.device.BluetoothDeviceId()).get();
    if (link.session) {
        link.session.MaintainConnection(true);
        link.chunk.store(chunkOf(link.session.MaxPduSize()), std::memory_order_release);
        link.sizeToken = link.session.MaxPduSizeChanged([weak](const gatt::GattSession& sender, const IInspectable&) {
            const std::shared_ptr<Link> alive = weak.lock();
            if (alive == nullptr) {
                return;
            }
            try {
                alive->chunk.store(chunkOf(sender.MaxPduSize()), std::memory_order_release);
            } catch (...) {
                return;
            }
        });
    }
    const gatt::GattDeviceServicesResult services =
        link.device.GetGattServicesForUuidAsync(guidOf(kSerialService), bluetooth::BluetoothCacheMode::Uncached).get();
    if (services.Status() != gatt::GattCommunicationStatus::Success) {
        return failureOf(services.Status());
    }
    if (services.Services().Size() == 0) {
        return BleFailure::NoService;
    }
    link.service = services.Services().GetAt(0);
    const gatt::GattCharacteristicsResult receive =
        link.service.GetCharacteristicsForUuidAsync(guidOf(kSerialReceive), bluetooth::BluetoothCacheMode::Uncached)
            .get();
    const gatt::GattCharacteristicsResult send =
        link.service.GetCharacteristicsForUuidAsync(guidOf(kSerialSend), bluetooth::BluetoothCacheMode::Uncached).get();
    if (receive.Status() != gatt::GattCommunicationStatus::Success) {
        return failureOf(receive.Status());
    }
    if (send.Status() != gatt::GattCommunicationStatus::Success) {
        return failureOf(send.Status());
    }
    if (receive.Characteristics().Size() == 0 || send.Characteristics().Size() == 0) {
        return BleFailure::NoService;
    }
    link.receive = receive.Characteristics().GetAt(0);
    link.send = send.Characteristics().GetAt(0);
    link.valueToken = link.send.ValueChanged(
        [weak](const gatt::GattCharacteristic&, const gatt::GattValueChangedEventArgs& args) {
            const std::shared_ptr<Link> alive = weak.lock();
            if (alive == nullptr || alive->ended.load(std::memory_order_acquire) || !alive->events.received) {
                return;
            }
            try {
                const std::vector<std::uint8_t> bytes = bytesOf(args.CharacteristicValue());
                alive->events.received(bytes);
            } catch (...) {
                return;
            }
        });
    const gatt::GattCommunicationStatus subscribed = link.send.WriteClientCharacteristicConfigurationDescriptorAsync(
        gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify).get();
    if (subscribed != gatt::GattCommunicationStatus::Success) {
        return failureOf(subscribed);
    }
    link.statusToken = link.device.ConnectionStatusChanged(
        [weak](const bluetooth::BluetoothLEDevice& sender, const IInspectable&) {
            try {
                if (sender.ConnectionStatus() != bluetooth::BluetoothConnectionStatus::Disconnected) {
                    return;
                }
            } catch (...) {
                return;
            }
            const std::shared_ptr<Link> alive = weak.lock();
            if (alive != nullptr && !alive->ended.exchange(true, std::memory_order_acq_rel) && alive->events.lost) {
                alive->events.lost();
            }
        });
    try {
        link.request = link.device.RequestPreferredConnectionParameters(
            bluetooth::BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
        const bool asked =
            link.request.Status() == bluetooth::BluetoothLEPreferredConnectionParametersRequestStatus::Success;
        log::info(kCategory, "the shortest Bluetooth connection interval was {}", asked ? "asked for" : "not granted");
    } catch (const winrt::hresult_error&) {
        log::info(kCategory, "this Windows cannot ask for a Bluetooth connection interval; the board asks for 7.5 ms");
    }
    return BleFailure::None;
}

class WinChannel final : public BleChannel
{
public:
    WinChannel(std::shared_ptr<Worker> worker, std::shared_ptr<Link> link)
        : m_worker(std::move(worker))
        , m_link(std::move(link))
    {
    }

    ~WinChannel() override
    {
        m_link->ended.store(true, std::memory_order_release);
        m_worker->post([link = std::move(m_link)] { closeLink(*link); });
    }

    WinChannel(const WinChannel&) = delete;
    WinChannel& operator=(const WinChannel&) = delete;

    [[nodiscard]] std::size_t chunk() const override { return m_link->chunk.load(std::memory_order_acquire); }

    [[nodiscard]] bool send(std::span<const std::uint8_t> bytes) override
    {
        if (m_link->ended.load(std::memory_order_acquire)) {
            return false;
        }
        try {
            const IBuffer buffer = CryptographicBuffer::CreateFromByteArray(
                winrt::array_view<const std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
            const gatt::GattCommunicationStatus status =
                m_link->receive.WriteValueAsync(buffer, gatt::GattWriteOption::WriteWithoutResponse).get();
            return status == gatt::GattCommunicationStatus::Success;
        } catch (const winrt::hresult_error& error) {
            log::warning(kCategory, "writing to the board over Bluetooth: {}", messageOf(error));
            return false;
        }
    }

private:
    std::shared_ptr<Worker> m_worker;
    std::shared_ptr<Link> m_link;
};

struct Seen {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::map<std::uint64_t, SeenBoard> boards;
    BluetoothState state = BluetoothState::Unknown;
    std::chrono::steady_clock::time_point stateAt{};
    bool refreshing = false;
    int watchers = 0;
    advertising::BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token token{};
};

void noteAdvert(Seen& seen, const advertising::BluetoothLEAdvertisementReceivedEventArgs& args)
{
    const std::uint64_t address = args.BluetoothAddress();
    const int signal = args.RawSignalStrengthInDBm();
    const advertising::BluetoothLEAdvertisement advertisement = args.Advertisement();
    std::string name = winrt::to_string(advertisement.LocalName());
    std::optional<BoardAdvert> advert;
    for (const advertising::BluetoothLEAdvertisementDataSection& section : advertisement.DataSections()) {
        if (section.DataType() != kServiceData128) {
            continue;
        }
        if (std::optional<BoardAdvert> found = advertOf(bytesOf(section.Data()))) {
            advert = std::move(found);
            break;
        }
    }
    {
        const std::scoped_lock lock(seen.mutex);
        if (!advert && !seen.boards.contains(address) && seen.boards.size() >= kMostSeen) {
            return;
        }
        SeenBoard& entry = seen.boards[address];
        entry.address = address;
        if (!name.empty()) {
            entry.name = std::move(name);
        }
        if (advert) {
            entry.advert = std::move(*advert);
        }
        entry.signal = signal;
        entry.at = std::chrono::steady_clock::now();
        if (seen.boards.size() > kMostSeen) {
            const auto oldest = std::ranges::min_element(
                seen.boards, {}, [](const std::pair<const std::uint64_t, SeenBoard>& each) { return each.second.at; });
            seen.boards.erase(oldest);
        }
    }
    seen.changed.notify_all();
}

void startWatching(const std::shared_ptr<Seen>& seen)
{
    try {
        if (!seen->watcher) {
            seen->watcher = advertising::BluetoothLEAdvertisementWatcher();
            seen->watcher.ScanningMode(advertising::BluetoothLEScanningMode::Active);
            seen->token = seen->watcher.Received(
                [weak = std::weak_ptr<Seen>(seen)](const advertising::BluetoothLEAdvertisementWatcher&,
                                                   const advertising::BluetoothLEAdvertisementReceivedEventArgs& args) {
                    const std::shared_ptr<Seen> alive = weak.lock();
                    if (alive == nullptr) {
                        return;
                    }
                    try {
                        noteAdvert(*alive, args);
                    } catch (...) {
                        return;
                    }
                });
        }
        seen->watcher.Start();
    } catch (const winrt::hresult_error& error) {
        log::warning(kCategory, "Bluetooth scanning did not start: {}", messageOf(error));
    }
}

void stopWatching(Seen& seen)
{
    try {
        if (seen.watcher) {
            seen.watcher.Stop();
        }
    } catch (const winrt::hresult_error& error) {
        log::info(kCategory, "Bluetooth scanning did not stop cleanly: {}", messageOf(error));
    }
}

void readState(Seen& seen)
{
    BluetoothState state = BluetoothState::Missing;
    try {
        const bluetooth::BluetoothAdapter adapter = bluetooth::BluetoothAdapter::GetDefaultAsync().get();
        if (adapter && adapter.IsLowEnergySupported()) {
            const radios::Radio radio = adapter.GetRadioAsync().get();
            state = radio && radio.State() == radios::RadioState::On ? BluetoothState::Ready : BluetoothState::Off;
        }
    } catch (const winrt::hresult_error& error) {
        log::info(kCategory, "the Bluetooth adapter could not be asked: {}", messageOf(error));
    }
    const std::scoped_lock lock(seen.mutex);
    seen.state = state;
    seen.stateAt = std::chrono::steady_clock::now();
    seen.refreshing = false;
}

bluetooth::BluetoothLEDevice deviceAt(std::uint64_t address)
{
    return bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
}

class WinRadio final : public BleRadio
{
public:
    WinRadio() = default;

    ~WinRadio() override
    {
        try {
            m_worker->call<bool>([seen = m_seen] {
                stopWatching(*seen);
                if (seen->watcher) {
                    seen->watcher.Received(seen->token);
                }
                seen->watcher = nullptr;
                return true;
            });
        } catch (...) {
            m_seen->watchers = 0;
        }
    }

    WinRadio(const WinRadio&) = delete;
    WinRadio& operator=(const WinRadio&) = delete;

    [[nodiscard]] BluetoothState state() override
    {
        bool refresh = false;
        BluetoothState current = BluetoothState::Unknown;
        {
            const std::scoped_lock lock(m_seen->mutex);
            current = m_seen->state;
            if (!m_seen->refreshing &&
                (current == BluetoothState::Unknown || std::chrono::steady_clock::now() - m_seen->stateAt > kStateAge)) {
                m_seen->refreshing = true;
                refresh = true;
            }
        }
        if (refresh) {
            m_worker->post([seen = m_seen] { readState(*seen); });
        }
        return current;
    }

    void watch(bool on) override
    {
        m_worker->post([seen = m_seen, on] {
            if (on) {
                if (seen->watchers++ == 0) {
                    startWatching(seen);
                }
            } else if (seen->watchers > 0 && --seen->watchers == 0) {
                stopWatching(*seen);
            }
        });
    }

    [[nodiscard]] std::vector<SeenBoard> seen(std::chrono::milliseconds within) override
    {
        std::vector<SeenBoard> found;
        const auto now = std::chrono::steady_clock::now();
        {
            const std::scoped_lock lock(m_seen->mutex);
            for (const auto& [address, entry] : m_seen->boards) {
                if (!entry.advert.id.empty() && now - entry.at <= within) {
                    found.push_back(entry);
                }
            }
        }
        std::ranges::sort(found, [](const SeenBoard& a, const SeenBoard& b) { return a.signal > b.signal; });
        return found;
    }

    [[nodiscard]] bool find(std::uint64_t address, std::chrono::milliseconds timeout,
                            const std::stop_token& stop) override
    {
        const auto fresh = [this, address] {
            const auto found = m_seen->boards.find(address);
            return found != m_seen->boards.end() && !found->second.advert.id.empty() &&
                   std::chrono::steady_clock::now() - found->second.at <= kSeenRecently;
        };
        {
            const std::scoped_lock lock(m_seen->mutex);
            if (fresh()) {
                return true;
            }
        }
        watch(true);
        bool found = false;
        {
            std::unique_lock lock(m_seen->mutex);
            found = m_seen->changed.wait_for(lock, stop, timeout, fresh);
        }
        watch(false);
        return found;
    }

    [[nodiscard]] bool linked(std::uint64_t address) override
    {
        return m_worker->call<bool>([address] {
            try {
                const bluetooth::BluetoothLEDevice device = deviceAt(address);
                return device && device.ConnectionStatus() == bluetooth::BluetoothConnectionStatus::Connected;
            } catch (const winrt::hresult_error&) {
                return false;
            }
        });
    }

    [[nodiscard]] bool paired(std::uint64_t address) override
    {
        return m_worker->call<bool>([address] {
            try {
                const bluetooth::BluetoothLEDevice device = deviceAt(address);
                return device && device.DeviceInformation().Pairing().IsPaired();
            } catch (const winrt::hresult_error&) {
                return false;
            }
        });
    }

    [[nodiscard]] PairOutcome pair(std::uint64_t address) override
    {
        return m_worker->call<PairOutcome>([address] {
            try {
                const bluetooth::BluetoothLEDevice device = deviceAt(address);
                if (!device) {
                    return PairOutcome::NotFound;
                }
                const enumeration::DeviceInformationPairing pairing = device.DeviceInformation().Pairing();
                if (pairing.IsPaired()) {
                    return PairOutcome::Paired;
                }
                if (!pairing.CanPair()) {
                    return PairOutcome::Refused;
                }
                const enumeration::DeviceInformationCustomPairing custom = pairing.Custom();
                const winrt::event_token token = custom.PairingRequested(
                    [](const enumeration::DeviceInformationCustomPairing&,
                       const enumeration::DevicePairingRequestedEventArgs& request) { request.Accept(); });
                const enumeration::DevicePairingResult result =
                    custom.PairAsync(enumeration::DevicePairingKinds::ConfirmOnly,
                                     enumeration::DevicePairingProtectionLevel::Encryption)
                        .get();
                custom.PairingRequested(token);
                const enumeration::DevicePairingResultStatus status = result.Status();
                if (status == enumeration::DevicePairingResultStatus::Paired ||
                    status == enumeration::DevicePairingResultStatus::AlreadyPaired) {
                    return PairOutcome::Paired;
                }
                log::warning(kCategory, "Windows did not pair the board (status {})", static_cast<int>(status));
                const bool refused = status == enumeration::DevicePairingResultStatus::RejectedByHandler ||
                                     status == enumeration::DevicePairingResultStatus::ConnectionRejected ||
                                     status == enumeration::DevicePairingResultStatus::AccessDenied ||
                                     status == enumeration::DevicePairingResultStatus::AuthenticationFailure ||
                                     status == enumeration::DevicePairingResultStatus::ProtectionLevelCouldNotBeMet;
                return refused ? PairOutcome::Refused : PairOutcome::Failed;
            } catch (const winrt::hresult_error& error) {
                log::warning(kCategory, "pairing over Bluetooth: {}", messageOf(error));
                return PairOutcome::Failed;
            }
        });
    }

    [[nodiscard]] bool unpair(std::uint64_t address) override
    {
        return m_worker->call<bool>([address] {
            try {
                const bluetooth::BluetoothLEDevice device = deviceAt(address);
                if (!device) {
                    return false;
                }
                const enumeration::DeviceUnpairingResultStatus status =
                    device.DeviceInformation().Pairing().UnpairAsync().get().Status();
                return status == enumeration::DeviceUnpairingResultStatus::Unpaired ||
                       status == enumeration::DeviceUnpairingResultStatus::AlreadyUnpaired;
            } catch (const winrt::hresult_error& error) {
                log::warning(kCategory, "forgetting the Bluetooth pairing: {}", messageOf(error));
                return false;
            }
        });
    }

    [[nodiscard]] std::unique_ptr<BleChannel> open(std::uint64_t address, BleChannelEvents events,
                                                   BleFailure& failure) override
    {
        auto link = std::make_shared<Link>();
        link->events = std::move(events);
        failure = m_worker->call<BleFailure>([link, address] {
            try {
                return connectLink(*link, address, std::weak_ptr<Link>(link));
            } catch (const winrt::hresult_error& error) {
                log::warning(kCategory, "connecting over Bluetooth: {}", messageOf(error));
                return BleFailure::Unreachable;
            }
        });
        if (failure != BleFailure::None) {
            m_worker->post([link] { closeLink(*link); });
            return nullptr;
        }
        return std::make_unique<WinChannel>(m_worker, std::move(link));
    }

private:
    std::shared_ptr<Seen> m_seen = std::make_shared<Seen>();
    std::shared_ptr<Worker> m_worker = std::make_shared<Worker>();
};

}

std::shared_ptr<BleRadio> systemBluetooth()
{
    return std::make_shared<WinRadio>();
}

}
