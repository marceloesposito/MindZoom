#pragma once

// Codici di stato e d'errore, in linguaggio naturale.
//
// Servono a chi presidia una postazione in mostra, non a chi ha scritto il
// programma: ogni condizione ha un codice breve (MZ-xNN), un titolo che dice
// COSA succede e un'azione che dice COSA FARE, e lo stesso codice compare
// tanto nel pannello operatore quanto nel log di diagnostica - anche in quello
// che resta su disco dopo un crash. Chi telefona per chiedere aiuto legge il
// codice; chi risponde sa gia' di cosa si tratta.
//
// Classi:
//   MZ-00   tutto regolare
//   MZ-Bxx  Bluetooth / collegamento con la fascia
//   MZ-Sxx  qualita' del segnale EEG
//   MZ-Cxx  calibrazione (solo modalita' classica)
//   MZ-Rxx  rendering / risorse / prestazioni
//   MZ-Axx  avvio
//   MZ-Ixx  informativi (non sono problemi)
//   MZ-X01  crash del programma (compare solo nel log)
//
// Header-only e senza dipendenze di piattaforma, cosi' lo shell Windows potra'
// usarlo tale e quale quando verra' portato.

#include "app/shared_state.hpp"

namespace mz::app::diag {

enum class Severity { Ok, Info, Warn, Error };

enum class Code {
    Ok = 0,
    // Bluetooth / fascia
    BleOff,
    BleScanning,
    BleConnecting,
    BleNoPackets,
    BleNoValidPackets,
    StreamStalled,
    // segnale
    SignalStale,
    ContactLost,
    Mains,
    Railing,
    Flat,
    NotASignal,
    Artifact,
    // calibrazione classica
    CalibNoSignal,
    CalibNotASignal,
    CalibWeak,
    // rendering / prestazioni
    LowFps,
    ImagesMissing,
    RenderInit,
    // avvio
    StartFailed,
    // informativi
    Replay,
    Warmup,
    Landing,
    // crash
    Crash,
};

struct Info {
    const char*    code;     // "MZ-B04"
    const wchar_t* title;    // cosa succede, in una riga
    const wchar_t* action;   // cosa fare, in una riga (vuoto se niente)
    Severity       severity;
};

inline const Info& info(Code c) noexcept {
    // clang-format off
    static const Info kTable[] = {
        {"MZ-00",  L"Tutto regolare", L"", Severity::Ok},

        {"MZ-B01", L"Fascia EEG non collegata",
                   L"Accendi la fascia, poi premi B e C per collegarla.", Severity::Error},
        {"MZ-B02", L"Ricerca della fascia EEG in corso",
                   L"Attendi. Se non si collega entro 30 s: fascia accesa e carica? Bluetooth del Mac attivo?", Severity::Warn},
        {"MZ-B03", L"Connessione alla fascia in corso",
                   L"Attendi qualche secondo.", Severity::Warn},
        {"MZ-B04", L"Fascia collegata ma nessun dato in arrivo",
                   L"Spegni e riaccendi la fascia, poi premi B e C per ricollegarla.", Severity::Error},
        {"MZ-B05", L"Dati in arrivo ma non riconosciuti come EEG",
                   L"Premi B, X (scollega) e poi C (ricollega). Se persiste: modello di fascia non supportato.", Severity::Error},
        {"MZ-B06", L"Flusso di dati interrotto, ripristino automatico in corso",
                   L"Attendi 5 s. Se si ripete, avvicina la fascia al computer.", Severity::Warn},

        {"MZ-S01", L"Nessun dato recente dalla fascia",
                   L"Controlla il collegamento (B). Se la fascia e' spenta, accendila.", Severity::Error},
        {"MZ-S02", L"Contatto degli elettrodi assente",
                   L"Sistema la fascia sulla fronte e inumidisci i contatti dietro le orecchie.", Severity::Warn},
        {"MZ-S03", L"Interferenza di rete a 50 Hz: un elettrodo non tocca la pelle",
                   L"Scosta i capelli sotto la fascia, spostala di qualche millimetro, inumidisci i contatti.", Severity::Warn},
        {"MZ-S04", L"Amplificatore saturo",
                   L"Fascia troppo stretta o elettrodo che sfrega: risistemala e attendi 3 s.", Severity::Warn},
        {"MZ-S05", L"Canale piatto: elettrodo staccato",
                   L"Controlla i contatti sulla fronte. Se la fascia e' scarica, mettila in carica.", Severity::Warn},
        {"MZ-S06", L"Il segnale ricevuto non e' un'onda cerebrale",
                   L"Spegni e riaccendi la fascia. Se persiste, riavvia il programma (R tenuto premuto).", Severity::Error},
        {"MZ-S07", L"Movimento rilevato: lettura momentaneamente sospesa",
                   L"Passa da solo: chiedi di restare fermi qualche secondo.", Severity::Info},

        {"MZ-C01", L"Calibrazione non riuscita: segnale assente",
                   L"Sistema la fascia e premi INVIO per riprovare (M: profilo generico).", Severity::Error},
        {"MZ-C02", L"Calibrazione non riuscita: il segnale non e' un'onda cerebrale",
                   L"Spegni e riaccendi la fascia, poi INVIO per riprovare.", Severity::Error},
        {"MZ-C03", L"Calibrazione non riuscita: variazione troppo debole",
                   L"Chiedi di marcare di piu' la differenza fra concentrazione e rilassamento, poi INVIO.", Severity::Warn},

        {"MZ-R01", L"Frequenza di aggiornamento bassa",
                   L"Chiudi gli altri programmi e collega l'alimentazione. Se persiste, riavvia il Mac.", Severity::Warn},
        {"MZ-R02", L"Immagini non trovate accanto al programma",
                   L"La cartella assets deve stare dentro MindZoom.app: ricopia l'app dalla chiavetta.", Severity::Error},
        {"MZ-R03", L"Impossibile inizializzare la grafica",
                   L"Riavvia il Mac. Se persiste, prova un altro computer.", Severity::Error},

        {"MZ-A01", L"Avvio non riuscito",
                   L"Leggi il dettaglio nella finestra e nel log; poi riprova dal launcher.", Severity::Error},

        {"MZ-I01", L"Sessione dimostrativa: segnale registrato, fascia non in uso",
                   L"Per usare la fascia: premi B e poi C.", Severity::Info},
        {"MZ-I02", L"Adattamento al segnale in corso",
                   L"Nessuna azione: termina da solo in circa 20 s.", Severity::Info},
        {"MZ-I03", L"Pagina d'ingresso: in attesa del partecipante",
                   L"INVIO per iniziare l'esperienza.", Severity::Info},

        {"MZ-X01", L"Il programma si e' chiuso per un errore interno",
                   L"Riavvialo dal launcher. Se si ripete, conserva questo file di log.", Severity::Error},
    };
    // clang-format on
    return kTable[static_cast<int>(c)];
}

/**
 * La condizione piu' importante fra quelle attive: e' quella che il pannello
 * mostra per prima e che il log annota quando cambia. L'ordine e' quello in
 * cui un operatore deve affrontarle: prima il collegamento (senza, il resto
 * non ha senso), poi il segnale, poi le prestazioni, infine le informative.
 */
inline Code primary(const ControlState& st, bool landing, bool lowFps) noexcept {
    if (!st.replaying) {
        switch (st.bleState) {
            case 0: return Code::BleOff;
            case 1: return Code::BleScanning;
            case 2: return Code::BleConnecting;
            default: break;
        }
        if (st.rawPackets == 0)   return Code::BleNoPackets;
        if (st.validPackets == 0) return Code::BleNoValidPackets;
    }
    if (st.stalled) return Code::StreamStalled;

    if (!st.signalFresh && st.frames == 0 && landing) {
        // Appena collegata: i primi dati devono ancora arrivare, non e' un
        // guasto.
    } else if (!st.signalFresh) {
        return Code::SignalStale;
    }

    switch (st.signalFault) {
        case 1: return Code::Mains;
        case 2: return Code::Railing;
        case 3: return Code::Flat;
        case 4: return Code::NotASignal;
        default: break;
    }
    if (st.signalFresh && !st.contactOk) return Code::ContactLost;

    if (st.calibStage != 0 && st.failReason != 0) {
        switch (st.failReason) {
            case 1: return Code::CalibNoSignal;
            case 3: return Code::CalibNotASignal;
            default: return Code::CalibWeak;
        }
    }

    if (lowFps) return Code::LowFps;
    if (st.signalFresh && st.artifact) return Code::Artifact;

    if (landing) return Code::Landing;
    if (st.replaying) return Code::Replay;
    if (st.adaptiveActive && !st.adaptiveReady) return Code::Warmup;
    return Code::Ok;
}

} // namespace mz::app::diag
