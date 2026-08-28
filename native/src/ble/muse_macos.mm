// Backend macOS del client Muse: CoreBluetooth al posto di WinRT GATT.
//
// Stessi UUID, stessa sequenza di avvio, stesso formato di comando, stessa
// politica di riaggancio del backend Windows. Cambia solo il modo in cui il
// sistema operativo consegna le notifiche.
//
// Differenza strutturale rispetto a WinRT: CoreBluetooth e' interamente a
// callback su una coda, non offre nulla di sincrono e non si puo' aspettare una
// risposta bloccando. Il backend Windows scandiva la sequenza di avvio con
// sleep e attese sulla risposta di controllo; qui la stessa sequenza si esprime
// come una catena di blocchi ritardati sulla coda del delegate. Il risultato
// osservabile e' identico, la forma no - ed e' inevitabile.
//
// ATTENZIONE: non e' mai stato compilato. Scritto su Windows, dove non esiste un
// toolchain Objective-C ne' CoreBluetooth. La logica di protocollo e' quella
// verificata sul campo; la sintassi Objective-C no.
//
// Nota di distribuzione: da macOS 11 l'accesso al Bluetooth richiede la chiave
// NSBluetoothAlwaysUsageDescription nell'Info.plist, altrimenti il processo
// viene terminato al primo uso di CBCentralManager. Vedi macos/Info.plist.

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "ble/muse.hpp"
#include "dsp/decode.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace {

// UUID del protocollo Muse, identici a quelli del client JS e del backend Windows.
// Il servizio e' un UUID a 16 bit (0xFE8D) espanso con la Bluetooth Base UUID:
// e' quello che il dispositivo annuncia davvero, non un 273E come le
// characteristic - un valore diverso qui significa scansione e discoverServices
// che non trovano mai nulla (visto sul campo: vedi muse.cpp, kServiceUuid).
NSString* const kServiceUuid = @"0000FE8D-0000-1000-8000-00805F9B34FB";
NSString* const kControlUuid = @"273E0001-4C4D-454D-96BE-F03BAC821358";
NSString* const kEegUuid     = @"273E0013-4C4D-454D-96BE-F03BAC821358";

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---------------------------------------------------------------------------
// Delegate
// ---------------------------------------------------------------------------

@interface MZMuseDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, assign) mz::ble::MuseClient::Impl* owner;
@end

namespace mz::ble {

struct MuseClient::Impl {
    MuseClient&        self;
    MZMuseDelegate*    delegate = nil;
    CBCentralManager*  central  = nil;
    CBPeripheral*      peripheral = nil;
    CBCharacteristic*  controlChar = nil;
    CBCharacteristic*  eegChar     = nil;
    dispatch_queue_t   queue = nullptr;

    dsp::ChainLocator  locator;
    bool               wantConnection = false;

    // Sottoscrizione alle notifiche: la sequenza di avvio parte solo quando
    // ENTRAMBE sono confermate, come su Windows (che blocca su
    // WriteClientCharacteristicConfigurationDescriptorAsync prima di scrivere
    // qualunque comando).
    bool               controlNotifying = false;
    bool               eegNotifying     = false;

    // Handshake di avvio: stato dello step corrente. `startupGeneration` si
    // incrementa a ogni (ri)avvio della sequenza cosi' un ack o un timeout in
    // arrivo in ritardo da un tentativo precedente non trova piu' corrispondenza
    // e viene ignorato invece di far avanzare lo step sbagliato.
    std::size_t        startupIndex      = 0;
    unsigned           startupGeneration = 0;
    bool               startupAwaitingAck = false;

    explicit Impl(MuseClient& s) : self(s) {}

    void startScan();
    void sendCommand(const std::string& cmd);
    void maybeStartHandshake();
    void runStartupSequence();
    void startupStep();
    void startupAdvance();
    void onControlAck();
    void handleNotification(const std::uint8_t* data, std::size_t len);
    void setState(State s) { self.state_.store(s, std::memory_order_release); }
    void log(const std::string& m) { if (self.onLog_) self.onLog_(m); }
    void setDeviceName(std::string name) {
        std::lock_guard<std::mutex> lock(self.nameMutex_);
        self.deviceName_ = std::move(name);
    }
};

// ---------------------------------------------------------------------------

MuseClient::MuseClient() {
    impl_ = new Impl(*this);
    impl_->queue = dispatch_queue_create("mindzoom.ble", DISPATCH_QUEUE_SERIAL);

    MZMuseDelegate* d = [[MZMuseDelegate alloc] init];
    d.owner = impl_;
    impl_->delegate = d;
}

MuseClient::~MuseClient() {
    stop();
    delete impl_;
}

void MuseClient::start() {
    // CBCentralManager/CBPeripheral si possono toccare solo dalla coda passata
    // a initWithDelegate:queue: - e' la stessa su cui arrivano i delegate.
    // start()/stop() sono chiamati dal thread della UI (o dal DSP), mai da li':
    // senza dispatch_sync qui, l'assegnazione a impl_->central correrebbe senza
    // sincronizzazione con le callback che la leggono/mutano in parallelo -
    // osservato sul campo come SIGSEGV in cancelPeripheralConnection: (ARC che
    // fa retain su un puntatore scritto a meta' da un altro thread).
    dispatch_sync(impl_->queue, ^{
        impl_->wantConnection = true;
        if (!impl_->central) {
            // Un solo CBCentralManager per tutta la vita del processo, creato
            // qui la prima volta e mai piu' sostituito. Ricrearlo a ogni
            // riconnessione (come faceva questa funzione) lascia il vecchio
            // CBCentralManager senza piu' nessun riferimento forte proprio
            // mentre puo' avere ancora una didDiscoverPeripheral: in coda: se
            // arriva dopo che start() ha gia' installato il central nuovo,
            // scrive in owner->peripheral un CBPeripheral legato al central
            // vecchio (ormai deallocato). Alla riconnessione successiva stop()
            // passa quel peripheral orfano al central NUOVO in
            // cancelPeripheralConnection: - osservato sul campo (26/08) come
            // SIGSEGV in objc_retain dentro quella chiamata, tre volte, sempre
            // dopo riconnessioni ravvicinate. Il CBCentralManager non si puo'
            // usare prima che riporti PoweredOn: la scansione della primissima
            // volta parte dal callback centralManagerDidUpdateState.
            impl_->central = [[CBCentralManager alloc] initWithDelegate:impl_->delegate
                                                                  queue:impl_->queue];
            NSLog(@"[MZDIAG] start: creato CBCentralManager (unico per il processo)");
        } else if (impl_->central.state == CBManagerStatePoweredOn) {
            // Riconnessione: il central esiste gia' ed e' pronto, si riparte
            // subito dalla scansione invece che aspettare un
            // centralManagerDidUpdateState che con un central gia' esistente
            // non arriva piu' (fa gia' PoweredOn da quando il central e' nato).
            NSLog(@"[MZDIAG] start: central gia' pronto, riscansione diretta");
            impl_->startScan();
        }
        // Se il central esiste ma non e' ancora PoweredOn, non c'e' altro da
        // fare qui: wantConnection e' gia' true, sara' centralManagerDidUpdateState
        // a far partire la scansione appena lo stato cambia.
    });
    state_.store(State::Scanning, std::memory_order_release);
}

void MuseClient::stop() {
    if (!impl_) return;
    dispatch_sync(impl_->queue, ^{
        impl_->wantConnection = false;   // disconnessione voluta: niente riaggancio

        if (impl_->central) {
            [impl_->central stopScan];
            if (impl_->peripheral) [impl_->central cancelPeripheralConnection:impl_->peripheral];
        }
        impl_->peripheral  = nil;
        impl_->controlChar = nil;
        impl_->eegChar     = nil;
    });
    state_.store(State::Disconnected, std::memory_order_release);
}

void MuseClient::resumeStreaming() {
    // sendCommand tocca peripheral/controlChar: stessa regola, stessa coda.
    if (streaming()) dispatch_async(impl_->queue, ^{ impl_->sendCommand("d"); });
}

std::string MuseClient::deviceName() const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return deviceName_;
}

std::int64_t MuseClient::millisSinceLastPacket() const {
    const auto at = lastPacketAt_.load(std::memory_order_relaxed);
    return (at == 0) ? 0 : (nowMillis() - at);
}

// ---------------------------------------------------------------------------

void MuseClient::Impl::startScan() {
    setState(State::Scanning);
    [central scanForPeripheralsWithServices:@[[CBUUID UUIDWithString:kServiceUuid]]
                                    options:nil];
}

void MuseClient::Impl::sendCommand(const std::string& cmd) {
    if (!controlChar || !peripheral) return;

    // Formato: [len][ascii...] con ascii = cmd + '\n' e len = lunghezza ascii.
    // Identico al backend Windows: e' protocollo, non piattaforma.
    std::vector<std::uint8_t> buf;
    buf.reserve(cmd.size() + 2);
    buf.push_back(static_cast<std::uint8_t>(cmd.size() + 1));
    for (const char c : cmd) buf.push_back(static_cast<std::uint8_t>(c));
    buf.push_back(static_cast<std::uint8_t>('\n'));

    NSData* data = [NSData dataWithBytes:buf.data() length:buf.size()];

    // Il backend Windows (e il driver JS originale) scrive PRIMA senza
    // risposta, e ripiega sulla scrittura normale solo se il dispositivo non
    // la accetta - vedi il commento in muse.cpp. La characteristic di
    // controllo della fascia in pratica supporta solo WriteWithoutResponse:
    // forzare sempre WriteWithResponse (come faceva questa funzione) viene
    // rifiutato dal dispositivo - osservato sul campo come "scrittura
    // fallita" per ogni comando.
    const CBCharacteristicWriteType type =
        (controlChar.properties & CBCharacteristicPropertyWriteWithoutResponse)
            ? CBCharacteristicWriteWithoutResponse
            : CBCharacteristicWriteWithResponse;
    [peripheral writeValue:data forCharacteristic:controlChar type:type];
}

namespace {
struct StartupStep { const char* cmd; int afterMs; };
// Stessi comandi e stessi ritardi EXTRA del backend Windows (i ritardi fra un
// sendCommand e il successivo in runStartupSequence, li'). La differenza è che
// li' ogni sendCommand aspetta anche awaitControlResponse(400) PRIMA di questi
// ritardi: quella parte, non riproducibile con un timer fisso, e' cio' che
// startupStep/onControlAck ricostruiscono qui sotto.
constexpr StartupStep kStartupSteps[] = {
    {"v6",    100}, {"s",     100}, {"h",     100},
    {"p21",   200}, {"dc001",   0}, {"L1",    300},
    {"h",     100}, {"p1041", 200},
    {"dc001",   0}, {"L1",    200}, {"s",       0},
};
constexpr std::size_t kStartupStepCount = sizeof(kStartupSteps) / sizeof(kStartupSteps[0]);
} // namespace

void MuseClient::Impl::runStartupSequence() {
    NSLog(@"[MZDIAG] runStartupSequence: avvio handshake");
    startupIndex = 0;
    ++startupGeneration;
    startupStep();
}

/**
 * Manda il comando dello step corrente, poi aspetta la risposta vera sulla
 * characteristic di controllo (onControlAck) - o, se non arriva, un timeout di
 * sicurezza di 400ms: esattamente lo stesso valore e lo stesso ruolo di
 * awaitControlResponse(400) sul backend Windows. E' questa attesa a scandire
 * la sequenza, non il timer da solo: senza, i comandi partono troppo
 * ravvicinati e la fascia non avvia lo streaming (osservato sul campo - luce
 * blu lampeggiante fissa, nessun pacchetto riconosciuto).
 */
void MuseClient::Impl::startupStep() {
    if (startupIndex >= kStartupStepCount) {
        setState(State::Streaming);
        log("streaming avviato");
        return;
    }

    log("handshake " + std::to_string(startupIndex + 1) + "/" +
        std::to_string(kStartupStepCount) + ": '" + kStartupSteps[startupIndex].cmd + "'");
    sendCommand(kStartupSteps[startupIndex].cmd);
    startupAwaitingAck = true;

    const unsigned gen = startupGeneration;
    const std::size_t idx = startupIndex;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), queue, ^{
        // idx != startupIndex: questo timer e' di uno step precedente, gia'
        // avanzato per altra via (ack arrivato, o un altro timeout). Senza
        // questo controllo un timer rimasto in sospeso puo' confondersi con lo
        // step ATTUALE (stesso startupAwaitingAck condiviso) e farlo avanzare
        // in anticipo con un messaggio fuorviante - osservato sul campo.
        if (gen != startupGeneration || idx != startupIndex || !startupAwaitingAck) return;
        log("  -> timeout, nessun ack per '" + std::string(kStartupSteps[idx].cmd) + "'");
        startupAdvance();
    });
}

/** Chiamato da didUpdateValueForCharacteristic quando risponde la characteristic di controllo. */
void MuseClient::Impl::onControlAck() {
    if (!startupAwaitingAck) return;
    log("  -> ack ricevuto");
    startupAdvance();
}

void MuseClient::Impl::startupAdvance() {
    startupAwaitingAck = false;
    const int  extraMs = kStartupSteps[startupIndex].afterMs;
    ++startupIndex;

    const unsigned gen = startupGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, extraMs * NSEC_PER_MSEC), queue, ^{
        if (gen != startupGeneration) return;
        startupStep();
    });
}

/** La sequenza parte solo quando ENTRAMBE le notifiche sono confermate attive. */
void MuseClient::Impl::maybeStartHandshake() {
    if (controlNotifying && eegNotifying) runStartupSequence();
}

void MuseClient::Impl::handleNotification(const std::uint8_t* data, std::size_t len) {
    self.rawPackets_.fetch_add(1, std::memory_order_relaxed);

    // Prima di ogni interpretazione: quello che il dispositivo ha davvero
    // mandato. Va consegnato anche per i pacchetti che verranno scartati.
    if (self.onRaw_ && len > 0) self.onRaw_(data, len);

    if (len < dsp::kPacketHeaderBytes + 1) return;

    if (!locator.offer(data, len)) {
        self.packetLen_.store(static_cast<int>(len), std::memory_order_relaxed);
        return;
    }
    const std::size_t start = locator.offset();
    if (start >= len) return;

    self.packetLen_.store(static_cast<int>(len), std::memory_order_relaxed);

    MuseClient& owner = self;
    const auto res = dsp::forEachEegSample(
        data, len, start, [&owner](const std::uint16_t* adc, int numChannels) {
            Sample sample;
            const int n = (numChannels < config::kChannels) ? numChannels : config::kChannels;
            for (int ch = 0; ch < n; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                sample.adc[c] = adc[ch];
                sample.uv[c]  = dsp::toMicrovolts(dsp::centerSample(adc[ch]));
            }
            if (owner.onSample_) owner.onSample_(sample);
        });

    if (!res.sawEeg) return;
    self.validPackets_.fetch_add(1, std::memory_order_relaxed);
    self.packetSamples_.store(res.eegSamples, std::memory_order_relaxed);
    self.lastPacketAt_.store(nowMillis(), std::memory_order_relaxed);
}

} // namespace mz::ble

// ---------------------------------------------------------------------------

@implementation MZMuseDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    NSLog(@"[MZDIAG] centralManagerDidUpdateState: %ld", (long)central.state);
    if (!self.owner) return;
    if (central.state == CBManagerStatePoweredOn) {
        if (self.owner->wantConnection) self.owner->startScan();
    } else {
        self.owner->setState(mz::ble::State::Disconnected);
        self.owner->log("Bluetooth non disponibile");
    }
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*, id>*)advertisementData
                  RSSI:(NSNumber*)RSSI {
    NSLog(@"[MZDIAG] didDiscoverPeripheral: %@ RSSI=%@", peripheral.name, RSSI);
    if (!self.owner || self.owner->peripheral) return;   // gia' agganciato

    [central stopScan];
    self.owner->peripheral = peripheral;
    peripheral.delegate = self;

    self.owner->setDeviceName(peripheral.name ? peripheral.name.UTF8String : "Muse");

    self.owner->setState(mz::ble::State::Connecting);
    [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral {
    if (self.owner) self.owner->log("connesso, cerco i servizi");
    [peripheral discoverServices:@[[CBUUID UUIDWithString:kServiceUuid]]];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error {
    if (self.owner) {
        self.owner->log("connessione FALLITA: " +
                        std::string(error.localizedDescription.UTF8String));
    }
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error {
    if (self.owner) {
        self.owner->log("disconnesso" +
                        (error ? (": " + std::string(error.localizedDescription.UTF8String)) : ""));
    }
    if (!self.owner) return;
    self.owner->peripheral       = nil;
    self.owner->controlChar      = nil;
    self.owner->eegChar          = nil;
    self.owner->controlNotifying = false;
    self.owner->eegNotifying     = false;
    self.owner->setState(mz::ble::State::Disconnected);

    // Riaggancio automatico solo se la disconnessione non era voluta. Un solo
    // tentativo alla volta: due connect() concorrenti si sabotano a vicenda, ed
    // e' lo stesso difetto gia' corretto sul backend Windows.
    if (self.owner->wantConnection) self.owner->startScan();
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
    if (self.owner) {
        self.owner->log("servizi trovati: " + std::to_string(peripheral.services.count) +
                        (error ? (", errore: " + std::string(error.localizedDescription.UTF8String)) : ""));
    }
    if (error || peripheral.services.count == 0) return;
    for (CBService* s in peripheral.services) {
        [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:kControlUuid],
                                              [CBUUID UUIDWithString:kEegUuid]]
                                 forService:s];
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error {
    if (!self.owner) return;
    self.owner->log("characteristic trovate: " + std::to_string(service.characteristics.count) +
                    (error ? (", errore: " + std::string(error.localizedDescription.UTF8String)) : ""));
    if (error) return;

    for (CBCharacteristic* c in service.characteristics) {
        if ([c.UUID isEqual:[CBUUID UUIDWithString:kControlUuid]]) {
            self.owner->controlChar = c;
            [peripheral setNotifyValue:YES forCharacteristic:c];
        } else if ([c.UUID isEqual:[CBUUID UUIDWithString:kEegUuid]]) {
            self.owner->eegChar = c;
            [peripheral setNotifyValue:YES forCharacteristic:c];
        }
    }
    if (!self.owner->controlChar) self.owner->log("ATTENZIONE: characteristic controllo non trovata");
    if (!self.owner->eegChar) self.owner->log("ATTENZIONE: characteristic EEG non trovata");
    // La sequenza di avvio parte da didUpdateNotificationStateForCharacteristic,
    // quando ENTRAMBE le sottoscrizioni sono confermate attive - non da qui:
    // setNotifyValue e' asincrono, e su Windows l'equivalente
    // (WriteClientCharacteristicConfigurationDescriptorAsync) e' bloccante,
    // quindi la' i comandi partono solo a sottoscrizione gia' avvenuta.
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error {
    if (!self.owner) return;
    const bool isControl = [characteristic.UUID isEqual:[CBUUID UUIDWithString:kControlUuid]];
    self.owner->log(std::string("notifiche ") + (isControl ? "controllo" : "EEG") +
                    (error ? (" FALLITE: " + std::string(error.localizedDescription.UTF8String))
                           : " attive"));
    if (error) return;

    if (isControl) {
        self.owner->controlNotifying = true;
    } else if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kEegUuid]]) {
        self.owner->eegNotifying = true;
    }
    self.owner->maybeStartHandshake();
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error {
    if (!self.owner) return;
    if (error) {
        self.owner->log("errore notifica: " + std::string(error.localizedDescription.UTF8String));
        return;
    }
    if (!characteristic.value) return;

    if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kEegUuid]]) {
        NSData* d = characteristic.value;
        self.owner->handleNotification(static_cast<const std::uint8_t*>(d.bytes), d.length);
    } else if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kControlUuid]]) {
        // Risposta del Muse a un comando: fa avanzare l'handshake, esattamente
        // come awaitControlResponse sul backend Windows - vedi il commento in
        // Impl::startupStep.
        self.owner->onControlAck();
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error {
    if (error && self.owner) {
        self.owner->log("scrittura comando FALLITA: " +
                        std::string(error.localizedDescription.UTF8String));
    }
}

@end
