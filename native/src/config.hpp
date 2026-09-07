#pragma once

// Specchio 1:1 di config.js + le costanti di testa di app.js.
// Single source of truth del port: qui NON si re-inventa nulla, si trascrive.
// I valori marcati TUNE sono le manopole di feel, da ri-tarare sul campo.
//
// I valori marcati LIVE hanno una copia modificabile a runtime in
// control/tunables.hpp: qui restano i DEFAULT, che sono anche ciò a cui il
// tasto R riporta tutto.

#include <array>
#include <cstddef>

namespace mz::config {

// --- Acquisizione ---
inline constexpr int    kSampleRate   = 256;   // Hz, hardware Muse (non alzare)
inline constexpr int    kChannels     = 4;     // TP9, AF7, AF8, TP10
inline constexpr int    kFrontalA     = 1;     // AF7
inline constexpr int    kFrontalB     = 2;     // AF8

// --- Derivazione bipolare AF7 - AF8 ---
// L'indice si calcola sulla DIFFERENZA fra i due frontali, non sulla somma dei
// due canali presi singolarmente.
//
// Motivo, misurato il 2026-08-16 su 173 finestre di segnale reale: il ronzio di
// rete arriva in modo comune, cioe' identico sui due elettrodi - differenza di
// fase +1,4 gradi, rapporto di ampiezza 1,01, il 96% delle finestre entro
// +-30 gradi. Sottraendo si cancella.
//
//     AF7 da solo    rete 81,3%   banda 4-30:  58 uV^2
//     AF8 da solo    rete 88,4%   banda 4-30:  34 uV^2
//     AF7 - AF8      rete  8,0%   banda 4-30:  89 uV^2
//
// La potenza in banda AUMENTA invece di calare, e 89 e' circa 58+34: i due
// canali si sommano in potenza, il che vuol dire che il loro contenuto in banda
// e' scorrelato. Non si sta cancellando segnale cerebrale, si sta togliendo
// solo cio' che i due elettrodi hanno in comune, che e' il disturbo.
//
// NON usare il riferimento medio dei quattro canali: misurato, peggiora
// (rete 98,2%), perche' TP9 e TP10 saturano il 41% del tempo e mediarli
// significa iniettare la loro saturazione dentro i frontali.
inline constexpr int    kBipolar      = kChannels;      // indice del canale virtuale
inline constexpr int    kStftChannels = kChannels + 1;  // i 4 fisici + il bipolare

// --- STFT a finestra scorrevole ---
inline constexpr int    kStftWindow   = 256;   // 1 s @256 Hz: risolve theta a 4 Hz
inline constexpr int    kStftHop      = 48;    // -> ~5.33 Hz di update del controllo
inline constexpr int    kStftBins     = kStftWindow / 2;

// Rate del controllo e passo temporale corrispondente.
inline constexpr double kControlHz    = double(kSampleRate) / double(kStftHop);
inline constexpr double kControlDt    = 1.0 / kControlHz;

// --- Decodifica campioni EEG ---
// L'ADC del Muse emette unsigned a 14 bit centrati sul fondo scala / 2.
inline constexpr int    kAdcCenter    = 8192;
inline constexpr double kAdcToMicrovolts = 1450.0 / 16383.0;

// --- Bin dell'indice di Pope (1 bin = FS/N = 1 Hz) ---
inline constexpr int    kThetaLo = 4,  kThetaHi = 8;
inline constexpr int    kAlphaLo = 8,  kAlphaHi = 12;
inline constexpr int    kBetaLo  = 13, kBetaHi  = 30;

// --- Filtri (biquad, forma diretta I) ---
// y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
inline constexpr double kDcLeak = 0.02;        // predittore DC
inline constexpr double kLpB0 = 0.2066, kLpB1 = 0.4132, kLpB2 = 0.2066;
inline constexpr double kLpA1 = -0.3695, kLpA2 = 0.1958;

// Il notch di rete si CALCOLA da kMainsHz invece di essere una manciata di
// coefficienti fissi. Quelli portati dal JavaScript originale erano
//     b = 0.9391, -0.4024, 0.9391   a1 = -0.4024, a2 = 0.8782
// che misurati danno -22 dB a 55 Hz e appena -1 dB a 50 Hz: il notch cadeva
// esattamente in mezzo fra la rete europea e quella americana, dove non c'e'
// niente da togliere. Attenuava il 20% della potenza di rete, non il 99%.
//
// Q = 10 -> larghezza circa 5 Hz, che copre anche le derive della frequenza di
// rete. A 30 Hz, cioe' al bordo superiore della banda beta usata dall'indice,
// il guadagno resta 0.997: l'indice non se ne accorge.
inline constexpr double kNotchQ = 10.0;

// --- Calibrazione attiva ---
//
// La calibrazione NON e' a tempo. Finisce quando ha raccolto abbastanza
// informazione, che e' una cosa diversa dall'aver aspettato abbastanza: con la
// fascia che perde contatto, quindici secondi di orologio possono contenere due
// campioni utili o ottanta.
//
// Il conteggio che conta e' quello dei campioni INDIPENDENTI. L'indice e'
// filtrato con tau = kIndexTauS, quindi campioni consecutivi sono fortemente
// correlati e contano per meno di uno. Per un processo AR(1) con correlazione
// rho a ritardo 1 la dimensione campionaria efficace e'
//
//     n_eff = n * (1 - rho) / (1 + rho)
//
// che e' il numero di campioni indipendenti equivalenti. A 5,33 Hz con
// tau = 0,9 s la correlazione e' alta e n_eff risulta molto minore di n: i
// quindici secondi di prima valevano circa una decina di campioni indipendenti,
// non ottanta. Questo spiega perche' certe calibrazioni riuscivano e altre no
// pur durando lo stesso tempo.
inline constexpr double kCalibIntroMinS    = 1.0;
inline constexpr double kCalibDisplayEma   = 0.20;  // TUNE - posizione quadratino a 60 Hz
inline constexpr double kCalibConcFraction = 0.50;  // LIVE - saturazione al 50% di M->estremo
inline constexpr double kCalibDoneHoldS    = 1.6;
inline constexpr double kCalibMinSpanRel   = 0.08;  // TUNE - soglia di fallimento

// Campioni indipendenti richiesti per fase.
//
// 20 viene dal confronto che la calibrazione deve reggere: separare due stati
// con la statistica t di Welch a un valore t >= kCalibMinSeparationT. Con due
// fasi da n_eff campioni ciascuna e una separazione di d deviazioni standard,
//     t = d / sqrt(2 / n_eff)   ->   n_eff = 2 * (t / d)^2
// Per una differenza netta fra concentrazione e rilassamento (d = 1,5) e t = 4
// servono 14 campioni. Portato da 20 (il margine) a 14 (il minimo che regge il
// confronto): misurato sul campo che con segnale reale in Relax n_eff satura
// molto sotto 20 e non accumulandosi mai (per richiesta esplicita: i campioni
// non si azzerano ne' calano) restava sotto soglia per sempre. Il margine si
// paga in tempo di attesa, non in un'attesa infinita.
inline constexpr double kCalibTargetEffSamples = 14.0;

// Sotto questa soglia di campioni GREZZI (non effettivi) un massimo storico di
// separazione/campioni non si aggiorna: con pochissimi campioni la varianza si
// stima male, e un picco statistico enorme puo' uscire per puro rumore, non
// perche' le due fasi siano davvero separate. Misurato: senza questo minimo,
// un segnale identico in media fra le due fasi "riusciva" dopo appena una
// trentina di campioni per un picco casuale del t di Welch. Non e' un reset -
// gli accumulatori continuano a crescere sempre - e' solo il momento da cui in
// poi ci si puo' fidare di un massimo raggiunto.
inline constexpr int kCalibMinRawForLatch = 40;

// --- Respiro guidato (fase Relax, ora la prima): box breathing 4-4-4-4 ---
// Ogni lato del box (inspira / trattieni / espira / trattieni) dura
// kBreathBoxS; l'animazione dei cerchi concentrici e il traguardo della fase
// sono entrambi derivati da qui, cosi' restano sincronizzati per costruzione
// invece che per coincidenza fra due costanti separate.
//
// La fase NON si chiude a meta' respiro: si valuta solo a fine ciclo, se ha
// gia' abbastanza campioni indipendenti bene, altrimenti se ne fa un altro -
// vedi Calibration::tick(). Un solo ciclo (16s) basta quasi sempre, dato che
// kCalibTargetEffSamples arriva prima; i cicli in piu' sono l'eccezione, non
// la norma.
inline constexpr double kBreathBoxS      = 4.0;
inline constexpr double kBreathCycleS    = kBreathBoxS * 4.0;   // un ciclo intero, 16 s

// --- Accoglienza e congedo (mostra senza operatore accanto) ---
//
// Fra la pagina d'ingresso e l'esperienza c'e' una schermata di istruzioni:
// indossare la fascia, accenderla, respirare un ciclo di box breathing. Il
// pulsante per proseguire resta SPENTO per kOnboardReadyS e poi si accende
// con una dissolvenza: e' l'unico modo per far durare davvero le istruzioni
// il tempo di leggerle e di fare almeno mezzo respiro guidato - chi ha fretta
// preme INVIO appena entrato e si ritrova nell'esperienza senza fascia in
// testa. Non e' un tempo di caricamento travestito: la banda adattiva si
// scalda per conto suo, e questo tempo le va a favore.
inline constexpr double kOnboardReadyS = 8.0;
inline constexpr double kOnboardFadeS  = 0.8;    // dissolvenza del pulsante quando si abilita

// Il promemoria per il riavvio compare solo a esperienza avviata da un po':
// prima sarebbe un invito a interromperla appena cominciata.
inline constexpr double kRestartHintAfterS = 50.0;

// Quanto tenere premuto R per riavviare. Due secondi sono lunghi apposta: e'
// un gesto che butta via la sessione, e l'anello di avanzamento attorno al
// tasto ha bisogno di tempo per essere letto come "sto per fare qualcosa" e
// non come un lampo. Condiviso con lo shell (shell_macos.mm), che e' chi
// misura la pressione.
inline constexpr double kRestartHoldS = 2.0;

// Congedo dopo il riavvio: togliere e igienizzare la fascia per chi viene
// dopo. Passa da solo - a fine esperienza nessuno ha voglia di premere un
// altro tasto, e la persona ha le mani occupate dalla fascia.
inline constexpr double kOffboardS = 8.0;

// Schermata di istruzioni fra la prima e la seconda fase: ha la stessa forma
// dell'introduzione, ma passa da sola invece di chiedere un tasto - a meta'
// esercizio non si puo' pretendere che la persona tenga una mano sulla
// tastiera, tanto meno mentre le si sta chiedendo di rilassarsi. Non fa parte
// della misura.
//
// Salita da 2.5: quella era una pausa "di respiro" con due righe di testo,
// non una schermata da leggere. Ora ci sono un titolo, un'istruzione e il
// promemoria del passaggio successivo, e sotto i 6 secondi non si fa in tempo
// a leggerli senza fretta - che e' l'opposto di come si dovrebbe arrivare a
// una fase di rilassamento.
inline constexpr double kCalibPrepareS = 6.5;

// Separazione minima fra le due fasi, in unita' di errore standard, sotto la
// quale si continua a raccogliere invece di accontentarsi (vedi
// Calibration::tick - non e' piu' un fallimento, solo un traguardo che decide
// quando fermarsi prima del tempo massimo per fase).
//
// Sceso da 4.0: quel valore veniva dal caso "differenza netta" (d = 1,5 dev.
// standard fra le due fasi, vedi kCalibTargetEffSamples) ed e' un bersaglio
// statisticamente pulito ma STRETTO per una persona media al primo tentativo,
// con una fascia dry consumer - osservato sul campo: 5 calibrazioni su 5 mai
// arrivate in fondo per questo motivo, con segnale altrimenti valido. 2.5
// resta un requisito vero (p < 0.02, non rumore) ed e' protetto dal pavimento
// di campioni grezzi kCalibMinRawForLatch: si e' scelto di essere raggiungibili
// piuttosto che statisticamente ineccepibili, perche' una calibrazione che non
// finisce mai non e' "rigorosa", e' inutilizzabile.
inline constexpr double kCalibMinSeparationT = 2.5;

// --- Banda adattiva (alternativa alla calibrazione a due fasi) ---
//
// Vedi control/adaptive_band.hpp per il perche'. Qui solo i tre numeri che la
// governano.
//
// La finestra decide DUE cose in tensione fra loro, e va scelta guardando
// entrambe - misurate rigiocando tre sessioni reali con finestre diverse
// (sweep del 28/08, zoom out disponibile e spostamento netto):
//
//   finestra   out%          netto (in - out)
//     20 s     34-40%        30 / 13 / -14      la banda insegue troppo:
//     30 s     32-40%        43 / 33 /  -7      il netto va a zero, cioe' si
//                                               oscilla sul posto invece di
//                                               viaggiare in profondita'
//     45 s     29-40%        61 / 54 /   3      <- scelta
//     60 s     25-39%        79 / 70 /  11
//    150 s     15-34%       134 /140 /  40      la banda resta indietro
//                                               rispetto alla deriva: 81% del
//                                               tempo sopra il neutro, che e'
//                                               il difetto da cui si partiva
//
// Corta, il neutro sta sempre al centro ma il controllo "si abitua" a te in
// pochi secondi e non si va da nessuna parte: concentrarsi a lungo smette di
// portare avanti. Lunga, si viaggia ma torna il problema originale. 45 s
// tengono lo zoom out disponibile circa un terzo del tempo (era un sesto con
// la calibrazione) senza azzerare la possibilita' di andare in profondita'.
inline constexpr double kAdaptiveWindowS = 45.0;

// Prima di questo tempo di segnale utile il controllo resta fermo: e' il
// "riscaldamento". Non e' una calibrazione mascherata - non si chiede niente
// alla persona e non si puo' fallire - e' solo il tempo che serve perche' i
// percentili significhino qualcosa. Meno della meta' della finestra, cosi' si
// comincia presto e la banda si affina strada facendo.
inline constexpr double kAdaptiveWarmupS = 20.0;

// Estremi della banda, in percentili della distribuzione corrente. Non 0 e 100:
// gli estremi assoluti su una distribuzione a code pesanti come questa sono
// dominati da un singolo campione anomalo. 15/85 lasciano fuori le code e
// tengono la parte in cui l'indice vive davvero.
inline constexpr double kAdaptiveLoPercentile = 0.15;
inline constexpr double kAdaptiveHiPercentile = 0.85;

// --- Banda di ripiego, quando la calibrazione personale non e' mai partita ---
//
// Usata SOLO su richiesta esplicita dell'utente dalla schermata "calibrazione
// non riuscita" per assenza totale di segnale (tasto M): non sostituisce la
// misura personale, la sostituisce quando quella misura proprio non e' stata
// possibile, cosi' l'esperienza resta raggiungibile anche con una fascia che
// non ha mai dato un campione buono.
//
// ATTENZIONE alla portata reale di questi numeri: l'indice di Pope qui è
// ESPLICITAMENTE adimensionale e dipendente dall'hardware (somma di magnitudo
// FFT grezza su una derivazione bipolare specifica, non potenza normalizzata -
// vedi il commento su popeIndex in dsp/stft.cpp). Non esiste in letteratura un
// valore assoluto "medio" per QUESTA implementazione, quindi questi due numeri
// non sono una citazione diretta di uno studio: kCalibFallbackNeutral e' ancorato
// al valore che l'indice assume meccanicamente su rumore bianco con questi
// stessi bin (vedi il commento su kMinAutocorr1 piu' sotto, ~2.1 - un punto di
// partenza "nulla in particolare sta succedendo" verificabile nel codice,
// invece che inventato), e la semi-ampiezza del ±50% riflette l'ordine di
// grandezza con cui la letteratura sull'indice di engagement di Pope, Bogart e
// Bartolome (1995) e i lavori successivi che lo usano riportano la RISALITA
// relativa fra uno stato di riposo e uno di compito attivo (tipicamente un
// fattore 1,5-3x, mai un numero assoluto portabile fra hardware diversi). E'
// un ripiego onesto, non una misura: il controllo che ne risulta reagisce a
// un bersaglio generico, non alla persona che lo indossa.
inline constexpr double kCalibFallbackNeutral  = 2.1;
inline constexpr double kCalibFallbackSpanFrac = 0.5;  // ±50% attorno al neutro

// Primi campioni di ogni fase, scartati: e' il transitorio di reazione al
// prompt, non lo stato che si vuole misurare. Sostituisce il vecchio lead-in a
// tempo con lo stesso scopo.
inline constexpr int    kCalibLeadInSamples = 8;

// Rete di sicurezza: oltre questo tempo di segnale UTILE in una fase si
// procede comunque con quello che c'e' (vedi Calibration::tick/finalize - non
// e' piu' un fallimento). Senza un limite, con una modulazione debole la
// barra resterebbe ferma per sempre; in un'installazione pubblica, o solo per
// una persona che sta provando l'esperienza, e' inaccettabile.
//
// Sceso da 90: era pensato come rete di sicurezza per un fallimento vero, che
// giustificava un margine ampio prima di arrendersi. Ora che il tempo massimo
// non fa piu' fallire ma solo accontentarsi del meglio raccolto, tenerlo cosi'
// alto significa solo far aspettare fino a 3 minuti (due fasi + pausa) chi ha
// un segnale debole prima di essere comunque lasciato passare. 45 dimezza
// l'attesa peggiore senza toccare il caso comune, che chiude molto prima
// perche' governato da kCalibTargetEffSamples/kCalibMinSeparationT, non da
// questo tetto.
inline constexpr double kCalibMaxPhaseS = 45.0;

// --- Condizionamento dell'indice di Pope ---
// L'indice è un rapporto fra potenze di banda: ha code pesanti, un singolo
// campione anomalo passerebbe dritto in un EMA. Prima la mediana lo toglie,
// poi due poli in cascata smussano senza lasciare spigoli.
inline constexpr int    kIndexMedianTaps = 5;       // dispari; latenza (n-1)/2 campioni
// Sceso da 0.90: a 5.33 Hz un polo a 0.9s produce una correlazione a ritardo 1
// vicina a 1 (misurato sul campo: 0.9-0.99), che nella calibrazione conta come
// "quasi lo stesso campione ripetuto" e affossa n_eff indipendentemente da
// quanti campioni grezzi arrivano. Qui la mediana toglie gia' l'anomalo; il
// polo serve solo a smussare, non a introdurre un ritardo di quasi un secondo.
inline constexpr double kIndexTauS       = 0.40;    // TUNE - costante di tempo, non un alfa

// --- Controllo a estremi ---
inline constexpr double kExtremaGain     = 0.30;    // LIVE - velocità normalizzata massima
// Velocità con cui la banda locale insegue il segnale, in frazioni di span/s.
// Va letta INSIEME allo smoothing dell'indice: ora che `c` è molto più liscio,
// un decadimento alto rende la banda inerte (resta incollata a `c`, il gate
// vale 1 sempre) e con essa la tolleranza. Misurato su registrazione: a 0.35 la
// tolleranza non cambia nulla, a 0.10 sposta il tempo di controllo attivo dal
// 62% al 93%. Da qui il valore, più basso del precedente 0.35.
inline constexpr double kLocalDecay      = 0.10;    // LIVE
// Ampiezza della rampa di attivazione, in frazioni della semi-span M->estremo.
// È il compromesso fra "fasico e faticoso" (piccolo) e "continuo e facile"
// (grande): con 0 si torna esattamente al gradino di prima. Alzato da 0.18:
// attivare la concentrazione risultava troppo faticoso, a scapito di
// un'esperienza che deve restare stimolante.
inline constexpr double kLocalTolerance  = 0.24;    // LIVE
// Zona morta attorno al neutro, in frazioni di u: toglie la deriva a riposo
// senza reintrodurre una soglia netta.
inline constexpr double kNeutralDeadzone = 0.10;    // TUNE

// --- Smoothing della velocità ---
// Il controllo aggiorna a 5.3 Hz mentre il render gira a 60: senza smoothing si
// vedono gli scalini. Costanti di tempo, non alfa: indipendenti dal rate.
inline constexpr double kVelTauS       = 0.35;      // LIVE - a valle della legge di controllo
inline constexpr double kVelRenderTauS = 0.12;      // TUNE - interpolazione 5.3 Hz -> 60 Hz
inline constexpr double kVelStaleTauS  = 0.50;      // TUNE - decadimento a segnale assente

// Smoothing elastico: uno stadio in PIU', a monte della legge di controllo,
// applicato solo all'indice che guida lo zoom (non a quello che alimenta la
// misura della banda adattiva/calibrazione, che deve restare fedele al
// segnale vero). Tempo di risposta piu' lungo = un picco di concentrazione
// isolato non muove piu' lo zoom, serve tenerla per davvero: e' quello che
// rende facile restare fermi sul livello attuale. A due poli come
// IndexSmoother, per lo stesso motivo (niente spigoli). Tasto E/MAIUSC+E,
// LIVE come velTauS; 0 = disattivato (emaAlpha lo rende un passa-through).
inline constexpr double kElasticTauS = 0.6;         // LIVE - vedi sopra

// --- Rate control (INVARIATO rispetto al JS) ---
inline constexpr double kZoomSpeedFactor = 0.003;
inline constexpr double kFocusEasing     = 0.06;

// --- Detent / isteresi / hold ---
inline constexpr double kSnapStrength   = 0.06;
inline constexpr double kSnapVelFrac    = 0.25;
inline constexpr double kEnterHoldFrac  = 0.15;   // TUNE
inline constexpr double kBreakHoldFrac  = 0.32;   // TUNE (> kEnterHoldFrac)
inline constexpr double kLockDwellS     = 3.0;
inline constexpr double kLockDwellMult  = 1.25;   // TUNE
// L'aggancio richiede PERMANENZA sotto soglia: senza, un controllo che passa per
// zero fra un impulso e l'altro si riaggancia ad ogni frame e annulla il
// movimento appena ottenuto.
inline constexpr double kEnterHoldDwellS  = 0.90; // TUNE
// Dopo uno sgancio il lock resta disabilitato per questo tempo, così si ottiene
// una finestra di movimento utilizzabile invece di un singolo frame.
inline constexpr double kBreakRefractoryS = 1.50; // TUNE

// --- Gating artefatti / contatto ---
inline constexpr double kArtifactRelMult    = 2.5;   // TUNE
inline constexpr double kArtifactUvFloor    = 150.0; // TUNE
inline constexpr int    kArtifactPeakHistory = 48;   // ~9 s di picchi a 5.3 Hz
inline constexpr double kArtifactHoldMaxS   = 1.0;
inline constexpr double kContactStdMin      = 0.5;   // TUNE - canale piatto = staccato

// --- Plausibilità fisica del segnale ---
// Rispondono alla domanda che precede ogni gating: quello che arriva è un
// segnale biologico? Senza questi controlli una sessione intera si calibra su
// rumore e nessuno se ne accorge, perché l'indice di Pope su rumore bianco vale
// meccanicamente 17/8 = 2.1 e sembra un valore normale.
//
// L'autocorrelazione è la prova decisiva, e vale per QUALUNQUE grandezza fisica
// continua, non solo per l'EEG: a 256 Hz due campioni distano 4 ms, e il filtro
// anti-aliasing a monte di ogni ADC impedisce che siano indipendenti. È la
// condizione di Nyquist, non una proprietà del cervello.
//
// La soglia va tenuta BASSA. Un segnale con energia tutta sotto i 30 Hz darebbe
// circa cos(2*pi*30/256) = 0.74, ma la rete elettrica a 50 Hz contribuisce
// cos(2*pi*50/256) = 0.34: un EEG valido in una stanza rumorosa può scendere
// parecchio. Una soglia troppo alta manderebbe a inseguire un guasto
// inesistente. Sul campo si è osservato 0.02, quindi il margine resta enorme.
inline constexpr double kMinAutocorr1   = 0.25;  // TUNE - prudente di proposito
inline constexpr double kMaxRailFraction = 0.02; // oltre il 2% ai fondo scala = saturo
inline constexpr double kMinSpreadCounts = 2.0;  // sotto = canale piatto

// Quota di potenza attorno ai 50 Hz oltre la quale il canale sta leggendo la
// rete elettrica e non la persona.
//
// Serve perché l'autocorrelazione da sola NON distingue i due casi: un canale
// di solo ronzio vale 0.33, cioè sopra kMinAutocorr1, e passerebbe per buono.
// È successo davvero: misurato il 2026-08-16 su una sessione con la fascia
// appoggiata male, quattro canali su otto avevano fra l'87% e il 99% della
// potenza nei bin 49-51 Hz, con l'autocorrelazione a 0.332 su tutti - cioè
// esattamente cos(2*pi*50/256), la firma di una sinusoide pura a 50 Hz.
//
// Un elettrodo che tocca la pelle cortocircuita l'accoppiamento capacitivo con
// la rete: la quota crolla. Su quella stessa registrazione i canali con un
// minimo di contatto stavano sotto il 5%. La soglia a metà è larghissima.
inline constexpr double kMainsHz          = 50.0;  // Europa; 60 in Nord America

// Soglia scelta confrontando due popolazioni misurate il 2026-08-16: 21 finestre
// con la fascia sul tavolo (elettrodi flottanti) e 225 con la fascia indossata.
//
//   soglia   passano a vuoto   passano in testa
//     50%          4,8%              51,1%
//     75%          4,8%              58,2%   <- stessi falsi positivi, +7%
//     85%         19,0%              62,2%
//     90%         90,5%              69,3%   <- il criterio collassa
//
// Oltre l'80% i falsi positivi esplodono perche' le due distribuzioni si
// sovrappongono proprio li'. Il 75% e' il punto in cui si recupera il massimo
// senza pagare nulla.
//
// NOTA: non tentare di sostituirla con la potenza ASSOLUTA in banda 4-30 Hz.
// Provato e scartato: un elettrodo flottante ne raccoglie di piu' di uno
// indossato (mediana 37808 contro 22975 uV^2), perche' capta deriva e
// interferenza a banda larga. E' la QUOTA che separa, non la quantita'.
inline constexpr double kMaxMainsFraction = 0.75;

// --- Watchdog del flusso ---
inline constexpr double kEegWatchdogS     = 3.0;
inline constexpr int    kEegWatchdogRetry = 3;

// --- Fasi ---
inline constexpr bool   kEnableHookPhase = false;
inline constexpr bool   kEnableTimeLimit = false;
inline constexpr double kPhaseHookS      = 5.5;
inline constexpr double kPhaseHandoverS  = 5.0;
inline constexpr double kPhaseInteractiveS = 80.0;
inline constexpr double kPhaseOutroS     = 18.0;
inline constexpr double kHookAutoVelocity = 0.45;  // TUNE
inline constexpr double kOutroTargetFocus = 1.0;
inline constexpr double kOutroEasing      = 0.012; // TUNE

// --- Rendering ---
inline constexpr int kTotalImages = 8;             // /images/<kScaleLabels[i]>.webp
// Stesse foto di prima (stesso soggetto, stessa sequenza di ingrandimento),
// solo colorate e ridotte da 10 a 8: le due tagliate erano i frame 7 e 8, gli
// unici due con lo stesso identico ingrandimento (2441 e 2441, nessun
// dettaglio in piu' fra i due). Percio' i valori restano quelli gia' misurati
// sulla barra di scala stampata nelle foto originali (lunghezza in pixel
// della barra / larghezza dell'immagine, rispetto al valore in μm
// dell'etichetta), non ristimati sulle nuove: e' la stessa scala vera di
// prima, non una nuova plausibile.
inline constexpr std::array<int, kTotalImages> kScaleLabels = {
    19, 54, 111, 260, 605, 1302, 3125, 7129
};

// Larghezza di riferimento a cui si riferiscono gli ingrandimenti qui sopra.
//
// "1000x" non e' una lunghezza: e' un rapporto, e diventa una misura solo
// fissando rispetto a cosa. La convenzione classica del microscopio elettronico
// e' la larghezza della stampa fotografica, circa 100 mm. Da qui:
//
//     larghezza inquadrata = kReferenceWidthMm / ingrandimento
//
// che a 32x da' 3,1 mm e a 41000x da' 2,4 um - valori coerenti con immagini SEM
// reali. E' l'unica assunzione dietro la barra di scala: se le immagini
// originali portano il proprio campo inquadrato, si cambia questa costante (o si
// sostituisce la tabella con i valori veri) e la barra diventa esatta.
inline constexpr double kReferenceWidthMm = 100.0;

} // namespace mz::config
