#include "ble/muse.hpp"

#include "dsp/decode.hpp"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace mz::ble {
namespace {

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;

// UUID del protocollo Muse, identici a quelli del client JS.
constexpr winrt::guid kServiceUuid{
    0x0000fe8d, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
constexpr winrt::guid kControlUuid{
    0x273e0001, 0x4c4d, 0x454d, {0x96, 0xbe, 0xf0, 0x3b, 0xac, 0x82, 0x13, 0x58}};
constexpr winrt::guid kEegUuid{
    0x273e0013, 0x4c4d, 0x454d, {0x96, 0xbe, 0xf0, 0x3b, 0xac, 0x82, 0x13, 0x58}};

constexpr int  kMaxBackoffMs = 4000;
constexpr auto kScanTimeout  = std::chrono::seconds(20);

std::int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

const char* toString(State s) noexcept {
    switch (s) {
        case State::Disconnected: return "DISCONNESSO";
        case State::Scanning:     return "RICERCA";
        case State::Connecting:   return "CONNESSIONE";
        case State::Streaming:    return "STREAMING";
    }
    return "?";
}

// ---------------------------------------------------------------------------

struct MuseClient::Impl {
    explicit Impl(MuseClient& owner) : self(owner) {}

    MuseClient& self;

    std::thread       worker;
    std::atomic<bool> running{false};

    // Il riaggancio vive in QUESTO thread e solo qui. Nel client JS il bug era
    // proprio avere due tentativi concorrenti che si sabotavano: con un unico
    // thread proprietario la cosa è impossibile per costruzione.
    std::mutex              wakeMutex;
    std::condition_variable wake;
    bool                    disconnectedFlag = false;

    BluetoothLEDevice  device{nullptr};
    // Il service e la sessione vanno TENUTI in vita per tutta la connessione.
    // Lasciandoli scadere come temporanei, Windows considera il link non più
    // richiesto da nessuno e lo abbatte da solo dopo pochi secondi.
    GattDeviceService  service{nullptr};
    GattSession        session{nullptr};
    GattCharacteristic controlChar{nullptr};
    GattCharacteristic eegChar{nullptr};
    event_token        eegToken{};

    // Trova da sé dove comincia la catena di pacchetti dentro la notifica.
    // Toccato solo dal thread delle notifiche GATT.
    dsp::ChainLocator locator;
    event_token        controlToken{};
    event_token        statusToken{};

    // Risposte del Muse sulla characteristic di controllo. Il driver JS attende
    // una notifica dopo ogni comando: quell'attesa scandisce la sequenza di
    // avvio, e senza di essa il dispositivo non comincia a trasmettere.
    std::mutex              ctrlMutex;
    std::condition_variable ctrlCv;
    bool                    ctrlReceived = false;
    std::string             ctrlLast;

    bool awaitControlResponse(int timeoutMs);

    void log(const std::string& msg) {
        if (self.onLog_) self.onLog_(msg);
    }

    void setState(State s) { self.state_.store(s, std::memory_order_release); }

    void run();
    bool connectOnce();
    void teardown();
    bool sendCommand(std::string_view cmd);
    void handlePacket(const std::vector<std::uint8_t>& data);
    std::optional<std::uint64_t> scanForDevice();
    void signalDisconnected();
};

// ---------------------------------------------------------------------------

MuseClient::MuseClient() : impl_(new Impl(*this)) {}

MuseClient::~MuseClient() {
    stop();
    delete impl_;
}

std::string MuseClient::deviceName() const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return deviceName_;
}

std::int64_t MuseClient::millisSinceLastPacket() const {
    const std::int64_t at = lastPacketAt_.load(std::memory_order_acquire);
    return (at == 0) ? 0 : (nowMillis() - at);
}

void MuseClient::start() {
    if (impl_->running.exchange(true)) return;   // già avviato
    impl_->worker = std::thread([this] { impl_->run(); });
}

void MuseClient::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->signalDisconnected();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->teardown();
    state_.store(State::Disconnected, std::memory_order_release);
}

void MuseClient::resumeStreaming() {
    if (streaming()) impl_->sendCommand("d");
}

// ---------------------------------------------------------------------------

void MuseClient::Impl::signalDisconnected() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        disconnectedFlag = true;
    }
    wake.notify_all();
}

void MuseClient::Impl::run() {
    // Il thread possiede la propria apartment WinRT: le chiamate .get() qui
    // sotto sono lecite solo perché siamo in MTA e non sul thread della UI.
    init_apartment(apartment_type::multi_threaded);

    int attempt = 0;

    while (running.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(wakeMutex);
            disconnectedFlag = false;
        }

        bool ok = false;
        try {
            ok = connectOnce();
        } catch (winrt::hresult_error const& e) {
            log("connessione fallita: " + winrt::to_string(e.message()));
        } catch (...) {
            log("connessione fallita: errore sconosciuto");
        }

        if (!ok) {
            teardown();
            setState(State::Disconnected);

            if (!running.load(std::memory_order_acquire)) break;

            // Si ritenta all'infinito. Un tetto ai tentativi ha senso per uno
            // strumento da riga di comando, non per un'esperienza che deve
            // restare in piedi: una caduta momentanea non può chiudere la
            // sessione per sempre. Chi vuole smettere chiude l'applicazione.
            ++attempt;
            const int shift   = std::min(attempt - 1, 4);   // evita l'overflow
            const int delayMs = std::min(500 * (1 << shift), kMaxBackoffMs);
            log("nuovo tentativo fra " + std::to_string(delayMs) + " ms (tentativo " +
                std::to_string(attempt) + ")");

            std::unique_lock<std::mutex> lock(wakeMutex);
            wake.wait_for(lock, std::chrono::milliseconds(delayMs),
                          [this] { return !running.load(std::memory_order_acquire); });
            continue;
        }

        attempt = 0;   // sessione aperta: il contatore riparte

        // Si resta qui finché il device non cade o non si chiede lo stop.
        {
            std::unique_lock<std::mutex> lock(wakeMutex);
            wake.wait(lock, [this] {
                return disconnectedFlag || !running.load(std::memory_order_acquire);
            });
        }

        teardown();
        setState(State::Disconnected);

        if (running.load(std::memory_order_acquire)) {
            log("connessione persa: riaggancio in corso");
        }
    }

    teardown();
    setState(State::Disconnected);
}

std::optional<std::uint64_t> MuseClient::Impl::scanForDevice() {
    setState(State::Scanning);
    log("ricerca dell'headband Muse...");

    std::mutex              m;
    std::condition_variable cv;
    std::optional<std::uint64_t> found;
    std::string foundName;

    BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(BluetoothLEScanningMode::Active);
    // Il Muse annuncia il service fe8d: è lo stesso filtro usato dal client JS.
    watcher.AdvertisementFilter().Advertisement().ServiceUuids().Append(kServiceUuid);

    const auto token = watcher.Received(
        [&](BluetoothLEAdvertisementWatcher const&,
            BluetoothLEAdvertisementReceivedEventArgs const& args) {
            std::lock_guard<std::mutex> lock(m);
            if (found) return;
            found     = args.BluetoothAddress();
            foundName = winrt::to_string(args.Advertisement().LocalName());
            cv.notify_all();
        });

    watcher.Start();

    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, kScanTimeout, [&] {
            return found.has_value() || !running.load(std::memory_order_acquire);
        });
    }

    watcher.Received(token);
    watcher.Stop();

    if (found && !foundName.empty()) {
        std::lock_guard<std::mutex> lock(self.nameMutex_);
        self.deviceName_ = foundName;
    }
    if (!found) log("nessun Muse trovato entro il tempo di ricerca");
    return found;
}

bool MuseClient::Impl::connectOnce() {
    const auto address = scanForDevice();
    if (!address) return false;
    if (!running.load(std::memory_order_acquire)) return false;

    setState(State::Connecting);

    device = BluetoothLEDevice::FromBluetoothAddressAsync(*address).get();
    if (!device) {
        log("apertura del device fallita");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(self.nameMutex_);
        self.deviceName_ = winrt::to_string(device.Name());
    }

    // Caduta della connessione: sveglia il thread proprietario, che gestirà il
    // riaggancio. Nessun tentativo parte da dentro questo callback.
    statusToken = device.ConnectionStatusChanged(
        [this](BluetoothLEDevice const& d, winrt::Windows::Foundation::IInspectable const&) {
            if (d.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                signalDisconnected();
            }
        });

    const auto services = device.GetGattServicesForUuidAsync(kServiceUuid).get();
    if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0) {
        log("service Muse non trovato sul device");
        return false;
    }
    service = services.Services().GetAt(0);

    // MaintainConnection dice a Windows di tenere aperto il link finché la
    // sessione è viva. Senza, il sistema chiude la connessione appena la
    // ritiene inattiva e si vedono scollegamenti a ripetizione senza causa
    // apparente: è il comportamento predefinito, non un guasto della fascia.
    try {
        session = service.Session();
        if (session) session.MaintainConnection(true);
    } catch (...) {
        log("impossibile richiedere il mantenimento della connessione");
    }

    const auto controls = service.GetCharacteristicsForUuidAsync(kControlUuid).get();
    if (controls.Status() != GattCommunicationStatus::Success ||
        controls.Characteristics().Size() == 0) {
        log("characteristic di controllo non trovata");
        return false;
    }
    controlChar = controls.Characteristics().GetAt(0);

    // Le risposte del Muse arrivano qui. Vanno sottoscritte PRIMA di mandare i
    // comandi, esattamente come fa il driver JS.
    controlToken = controlChar.ValueChanged(
        [this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            std::vector<std::uint8_t> data(reader.UnconsumedBufferLength());
            if (!data.empty()) reader.ReadBytes(data);

            // Primo byte = lunghezza, il resto è testo.
            std::string text;
            for (std::size_t i = 1; i < data.size(); ++i) {
                const char c = static_cast<char>(data[i]);
                if (c >= 32 && c < 127) text += c;
            }
            {
                std::lock_guard<std::mutex> lock(ctrlMutex);
                ctrlLast     = text;
                ctrlReceived = true;
            }
            ctrlCv.notify_all();
        });

    const auto ctrlNotify =
        controlChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                       GattClientCharacteristicConfigurationDescriptorValue::Notify)
            .get();
    if (ctrlNotify != GattCommunicationStatus::Success) {
        log("attivazione delle notifiche di controllo fallita");
        return false;
    }

    const auto eegs = service.GetCharacteristicsForUuidAsync(kEegUuid).get();
    if (eegs.Status() != GattCommunicationStatus::Success ||
        eegs.Characteristics().Size() == 0) {
        log("characteristic EEG non trovata");
        return false;
    }
    eegChar = eegs.Characteristics().GetAt(0);

    eegToken = eegChar.ValueChanged(
        [this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
            const auto buffer = args.CharacteristicValue();
            auto reader = DataReader::FromBuffer(buffer);
            std::vector<std::uint8_t> data(reader.UnconsumedBufferLength());
            if (!data.empty()) reader.ReadBytes(data);
            handlePacket(data);
        });

    const auto notifyStatus =
        eegChar.WriteClientCharacteristicConfigurationDescriptorAsync(
                   GattClientCharacteristicConfigurationDescriptorValue::Notify)
            .get();
    if (notifyStatus != GattCommunicationStatus::Success) {
        log("attivazione delle notifiche EEG fallita");
        return false;
    }

    // Sequenza di avvio dello streaming, identica al client JS.
    using namespace std::chrono_literals;
    sendCommand("v6");    std::this_thread::sleep_for(100ms);
    sendCommand("s");     std::this_thread::sleep_for(100ms);
    sendCommand("h");     std::this_thread::sleep_for(100ms);
    sendCommand("p21");   std::this_thread::sleep_for(200ms);
    sendCommand("dc001"); sendCommand("L1"); std::this_thread::sleep_for(300ms);
    sendCommand("h");     std::this_thread::sleep_for(100ms);
    sendCommand("p1041"); std::this_thread::sleep_for(200ms);
    sendCommand("dc001"); sendCommand("L1"); std::this_thread::sleep_for(200ms);
    sendCommand("s");

    setState(State::Streaming);
    log("streaming avviato (" + self.deviceName() + ")");
    return true;
}

void MuseClient::Impl::teardown() {
    try {
        if (eegChar && eegToken.value) eegChar.ValueChanged(eegToken);
        if (controlChar && controlToken.value) controlChar.ValueChanged(controlToken);
        if (device && statusToken.value) device.ConnectionStatusChanged(statusToken);
        if (session) { session.MaintainConnection(false); session.Close(); }
        if (service) service.Close();
        if (device)  device.Close();
    } catch (...) {
        // In chiusura un fallimento non ha rimedio utile: si prosegue.
    }
    eegToken     = {};
    controlToken = {};
    statusToken  = {};
    eegChar     = nullptr;
    controlChar = nullptr;
    session     = nullptr;
    service     = nullptr;
    device      = nullptr;
}

bool MuseClient::Impl::awaitControlResponse(int timeoutMs) {
    std::unique_lock<std::mutex> lock(ctrlMutex);
    ctrlReceived = false;
    return ctrlCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return ctrlReceived; });
}

bool MuseClient::Impl::sendCommand(std::string_view cmd) {
    if (!controlChar) return false;

    const auto build = [&] {
        // Formato: [len][ascii...] con ascii = cmd + '\n' e len = lunghezza ascii.
        DataWriter writer;
        writer.WriteByte(static_cast<std::uint8_t>(cmd.size() + 1));
        for (const char c : cmd) writer.WriteByte(static_cast<std::uint8_t>(c));
        writer.WriteByte(static_cast<std::uint8_t>('\n'));
        return writer.DetachBuffer();
    };

    {
        std::lock_guard<std::mutex> lock(ctrlMutex);
        ctrlReceived = false;
    }

    auto status = GattCommunicationStatus::Unreachable;
    try {
        status = controlChar.WriteValueAsync(build(),
                                             GattWriteOption::WriteWithoutResponse).get();
    } catch (...) {
        status = GattCommunicationStatus::Unreachable;
    }

    // Non tutte le implementazioni accettano la scrittura senza risposta: il
    // driver JS ripiega sulla scrittura normale, e qui serve lo stesso.
    if (status != GattCommunicationStatus::Success) {
        try {
            status = controlChar.WriteValueAsync(build(),
                                                 GattWriteOption::WriteWithResponse).get();
        } catch (...) {
            status = GattCommunicationStatus::Unreachable;
        }
    }

    if (status != GattCommunicationStatus::Success) {
        log("comando '" + std::string(cmd) + "' non inviato");
        return false;
    }

    // L'attesa della risposta scandisce la sequenza: senza, i comandi partono
    // troppo ravvicinati e il dispositivo non avvia lo streaming.
    awaitControlResponse(400);
    return true;
}

void MuseClient::Impl::handlePacket(const std::vector<std::uint8_t>& data) {
    // Contato prima di ogni filtro: se resta a zero le notifiche non arrivano
    // affatto, il che è un problema diverso dal formato non riconosciuto.
    self.rawPackets_.fetch_add(1, std::memory_order_relaxed);

    // Prima di ogni interpretazione: quello che il dispositivo ha davvero
    // mandato. Va consegnato anche per i pacchetti che verranno scartati -
    // quelli scartati sono spesso proprio quelli da capire.
    if (self.onRaw_ && !data.empty()) self.onRaw_(data.data(), data.size());

    const std::size_t len = data.size();
    if (len < dsp::kPacketHeaderBytes + 1) return;

    // Una notifica contiene PIÙ pacchetti concatenati, non uno solo, e il
    // prefisso di trasporto non è documentato in modo affidabile. L'offset non
    // si assume: si misura, osservando che in ogni notifica cade nello stesso
    // punto. Finché non è deciso non si emette nulla - sono due secondi, e un
    // offset sbagliato non fallisce, produce campioni plausibili e falsi.
    if (!locator.offer(data.data(), len)) {
        self.packetLen_.store(static_cast<int>(len), std::memory_order_relaxed);
        return;
    }
    const std::size_t start = locator.offset();
    if (start >= len) return;

    self.packetLen_.store(static_cast<int>(len), std::memory_order_relaxed);

    MuseClient& owner = self;
    const auto res = dsp::forEachEegSample(
        data.data(), len, start,
        [&owner](const std::uint16_t* adc, int numChannels) {
            // Degli 8 canali dell'Athena si usano i primi 4, che sono quelli
            // montati sulla fascia.
            Sample sample;
            const int n = (numChannels < config::kChannels) ? numChannels
                                                            : config::kChannels;
            for (int ch = 0; ch < n; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                sample.adc[c] = adc[ch];
                sample.uv[c]  = dsp::toMicrovolts(dsp::centerSample(adc[ch]));
            }
            if (owner.onSample_) owner.onSample_(sample);
        });

    const int emitted = res.eegSamples;
    if (!res.sawEeg) return;   // notifica valida ma senza EEG: ottica, IMU, batteria

    self.validPackets_.fetch_add(1, std::memory_order_relaxed);
    self.lastPacketAt_.store(nowMillis(), std::memory_order_release);
    self.packetSamples_.store(emitted, std::memory_order_relaxed);
}

} // namespace mz::ble
