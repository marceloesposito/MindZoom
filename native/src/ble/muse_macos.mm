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
NSString* const kServiceUuid = @"273E0000-4C4D-454D-96BE-F03BAC821358";
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

    explicit Impl(MuseClient& s) : self(s) {}

    void startScan();
    void sendCommand(const std::string& cmd);
    void runStartupSequence();
    void handleNotification(const std::uint8_t* data, std::size_t len);
    void setState(State s) { self.state_.store(s, std::memory_order_release); }
    void log(const std::string& m) { if (self.onLog_) self.onLog_(m); }
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
    impl_->wantConnection = true;
    // Il CBCentralManager non puo' essere usato prima che riporti PoweredOn:
    // la scansione parte dal callback centralManagerDidUpdateState.
    impl_->central = [[CBCentralManager alloc] initWithDelegate:impl_->delegate
                                                          queue:impl_->queue];
    state_.store(State::Scanning, std::memory_order_release);
}

void MuseClient::stop() {
    if (!impl_) return;
    impl_->wantConnection = false;   // disconnessione voluta: niente riaggancio

    if (impl_->central) {
        [impl_->central stopScan];
        if (impl_->peripheral) [impl_->central cancelPeripheralConnection:impl_->peripheral];
    }
    impl_->peripheral  = nil;
    impl_->controlChar = nil;
    impl_->eegChar     = nil;
    state_.store(State::Disconnected, std::memory_order_release);
}

void MuseClient::resumeStreaming() {
    if (streaming()) impl_->sendCommand("d");
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
    [peripheral writeValue:data
         forCharacteristic:controlChar
                      type:CBCharacteristicWriteWithResponse];
}

void MuseClient::Impl::runStartupSequence() {
    // Stessa sequenza e stessi ritardi del backend Windows. Li' erano sleep su un
    // thread dedicato, qui sono blocchi ritardati sulla coda BLE: CoreBluetooth
    // non ammette attese bloccanti sulla sua coda, e bloccarla impedirebbe di
    // ricevere proprio le risposte che si stanno aspettando.
    struct Passo { int ritardoMs; const char* cmd; };
    static const Passo passi[] = {
        {   0, "v6"    }, { 100, "s"     }, { 200, "h"     },
        { 300, "p21"   }, { 500, "dc001" }, { 500, "L1"    },
        { 800, "h"     }, { 900, "p1041" },
        {1100, "dc001" }, {1100, "L1"    }, {1300, "s"     },
    };

    for (const auto& p : passi) {
        const std::string cmd = p.cmd;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, p.ritardoMs * NSEC_PER_MSEC),
                       queue, ^{
            sendCommand(cmd);
        });
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), queue, ^{
        setState(State::Streaming);
        log("streaming avviato");
    });
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
    if (!self.owner || self.owner->peripheral) return;   // gia' agganciato

    [central stopScan];
    self.owner->peripheral = peripheral;
    peripheral.delegate = self;

    {
        std::lock_guard<std::mutex> lock(self.owner->self.nameMutex_);
        self.owner->self.deviceName_ =
            peripheral.name ? peripheral.name.UTF8String : "Muse";
    }

    self.owner->setState(mz::ble::State::Connecting);
    [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral {
    [peripheral discoverServices:@[[CBUUID UUIDWithString:kServiceUuid]]];
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error {
    if (!self.owner) return;
    self.owner->peripheral  = nil;
    self.owner->controlChar = nil;
    self.owner->eegChar     = nil;
    self.owner->setState(mz::ble::State::Disconnected);

    // Riaggancio automatico solo se la disconnessione non era voluta. Un solo
    // tentativo alla volta: due connect() concorrenti si sabotano a vicenda, ed
    // e' lo stesso difetto gia' corretto sul backend Windows.
    if (self.owner->wantConnection) self.owner->startScan();
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
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
    if (!self.owner || error) return;

    for (CBCharacteristic* c in service.characteristics) {
        if ([c.UUID isEqual:[CBUUID UUIDWithString:kControlUuid]]) {
            self.owner->controlChar = c;
            [peripheral setNotifyValue:YES forCharacteristic:c];
        } else if ([c.UUID isEqual:[CBUUID UUIDWithString:kEegUuid]]) {
            self.owner->eegChar = c;
            [peripheral setNotifyValue:YES forCharacteristic:c];
        }
    }

    if (self.owner->controlChar && self.owner->eegChar) {
        self.owner->runStartupSequence();
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error {
    if (!self.owner || error || !characteristic.value) return;

    if ([characteristic.UUID isEqual:[CBUUID UUIDWithString:kEegUuid]]) {
        NSData* d = characteristic.value;
        self.owner->handleNotification(static_cast<const std::uint8_t*>(d.bytes), d.length);
    }
    // Le risposte sulla characteristic di controllo sono JSON di stato: utili in
    // diagnostica, non necessarie al funzionamento, quindi non si interpretano.
}

@end
