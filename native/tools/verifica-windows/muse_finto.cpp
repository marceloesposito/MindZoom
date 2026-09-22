// Finto backend BLE, SOLO per la prova sotto Wine su un Mac. Non fa parte del
// progetto e non va nel repo: su Windows vero il file e' src/ble/muse.cpp, che
// usa C++/WinRT e richiede MSVC.
//
// Il selftest non usa la fascia - disegna un fotogramma e basta - quindi qui
// serve solo che i simboli esistano perche' il link riesca.

#include "ble/muse.hpp"

namespace mz::ble {

struct MuseClient::Impl {};

MuseClient::MuseClient() = default;
MuseClient::~MuseClient() = default;

void MuseClient::start() {}
void MuseClient::stop() {}
void MuseClient::resumeStreaming() {}

std::string MuseClient::deviceName() const { return "(finto)"; }
std::int64_t MuseClient::millisSinceLastPacket() const { return 0; }

} // namespace mz::ble
