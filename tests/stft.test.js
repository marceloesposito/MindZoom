// Verifica la STFT a finestra scorrevole e il gating, iniettando pacchetti BLE sintetici.
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const configSrc = fs.readFileSync(path.join(ROOT, 'config.js'), 'utf8');
const museSrc = fs.readFileSync(path.join(ROOT, 'MuseBluetooth.js'), 'utf8');

const sandbox = { console, Math, Date, Float32Array, Uint8Array, DataView, Array, Promise, window: {} };
vm.createContext(sandbox);
vm.runInContext(configSrc, sandbox);
vm.runInContext(museSrc, sandbox);

const CONFIG = sandbox.window.CONFIG;   // il `const` top-level non finisce su globalThis
const MuseBluetooth = sandbox.window.MuseBluetooth;

let pass = 0, fail = 0;
function check(name, cond, detail = '') {
    if (cond) { pass++; console.log(`  PASS  ${name}`); }
    else { fail++; console.log(`  FAIL  ${name}  ${detail}`); }
}

const HEADER = 14;
const SAMPLES_PER_PACKET = 2;
const CHANNELS = 4;

/** Impacchetta 8 valori a 14 bit nel formato letto da _get14BitRaw. */
function buildPacket(values) {
    const payloadBytes = 14;                 // 8 * 14 bit = 112 bit
    const buf = new Uint8Array(HEADER + payloadBytes);
    buf[9] = 0x01;                           // dataType = 1 -> 4 canali
    let bitOffset = 0;
    for (const v of values) {
        const raw = v & 0x3FFF;
        for (let k = 0; k < 14; k++) {
            const bit = (raw >> k) & 1;
            const idx = HEADER + ((bitOffset + k) >> 3);
            buf[idx] |= bit << ((bitOffset + k) & 7);
        }
        bitOffset += 14;
    }
    return new DataView(buf.buffer);
}

const UV_PER_LSB = 1450.0 / 16383.0;
// L'ADC emette valori unsigned centrati su 8192 (vedi _centerSample).
const ADC_CENTER = 8192;
function uvToRaw(uv) {
    return ADC_CENTER + Math.round(uv / UV_PER_LSB);
}

/**
 * Invia `nSamples` campioni di una sinusoide, restituendo gli emit osservati.
 * `dcLsb` simula l'offset DC reale del Muse (valori unsigned centrati su 8192).
 */
function runSignal(muse, nSamples, ampUv, freqHz, spikeAt = -1, spikeUv = 0, dcLsb = 0) {
    const emits = [];
    muse.onEEGData(d => emits.push({
        t: muse.totalSamples,
        artifact: d.quality.artifact,
        contactOk: d.quality.contactOk,
        maxAbsRaw: d.quality.maxAbsRaw
    }));

    for (let s = 0; s < nSamples; s += SAMPLES_PER_PACKET) {
        const values = [];
        for (let k = 0; k < SAMPLES_PER_PACKET; k++) {
            const n = s + k;
            let uv = ampUv * Math.sin(2 * Math.PI * freqHz * n / 256);
            if (spikeAt >= 0 && n >= spikeAt && n < spikeAt + 8) uv = spikeUv;
            const lsb = dcLsb ? dcLsb + Math.round(uv / UV_PER_LSB) : uvToRaw(uv);
            for (let ch = 0; ch < CHANNELS; ch++) values.push(lsb);
        }
        muse.handleIncomingEEGPacket('x', buildPacket(values));
    }
    return emits;
}

// ---------------------------------------------------------------
console.log('\n[1] Rate di emissione della STFT scorrevole');
let muse = new MuseBluetooth();
const N = 256 * 8;                       // 8 secondi
let emits = runSignal(muse, N, 30, 10);

// Primo emit solo a finestra piena, poi uno ogni STFT_HOP campioni.
const expected = Math.floor((N - CONFIG.STFT_WINDOW) / CONFIG.STFT_HOP) + 1;
check(`numero di emit ~ atteso (${emits.length} vs ${expected})`,
      Math.abs(emits.length - expected) <= 1, `got=${emits.length} exp=${expected}`);

check('nessun emit prima che la finestra sia piena',
      emits[0].t >= CONFIG.STFT_WINDOW, `primo emit a ${emits[0].t} campioni`);

const gaps = [];
for (let i = 1; i < emits.length; i++) gaps.push(emits[i].t - emits[i - 1].t);
const uniformGaps = gaps.every(g => g === CONFIG.STFT_HOP);
check(`hop costante di ${CONFIG.STFT_HOP} campioni`, uniformGaps,
      `gaps unici: ${[...new Set(gaps)].join(',')}`);

const effectiveHz = 256 / CONFIG.STFT_HOP;
check(`rate effettivo 4-8 Hz (${effectiveHz.toFixed(1)} Hz)`, effectiveHz >= 4 && effectiveHz <= 8);

// ---------------------------------------------------------------
console.log('\n[2] Gating artefatti (ampiezza grezza)');
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 6, 30, 10, 256 * 3, 400);   // spike a 400 µV

const clean = emits.filter(e => !e.artifact);
const dirty = emits.filter(e => e.artifact);
check('lo spike da 400 µV marca finestre come artefatto', dirty.length > 0,
      `dirty=${dirty.length}`);
check('esistono finestre pulite non marcate', clean.length > 0, `clean=${clean.length}`);
check('le finestre marcate superano il pavimento assoluto',
      dirty.every(e => e.maxAbsRaw > CONFIG.ARTIFACT_UV_FLOOR));
check('le finestre pulite restano sotto il pavimento',
      clean.every(e => e.maxAbsRaw <= CONFIG.ARTIFACT_UV_FLOOR));

// L'artefatto deve uscire dalla finestra scorrevole: il gating non resta appiccicato.
const lastEmits = emits.slice(-5);
check('il gating si libera quando lo spike esce dalla finestra',
      lastEmits.every(e => !e.artifact), `coda: ${lastEmits.map(e => e.artifact).join(',')}`);

// ---------------------------------------------------------------
// REGRESSIONE: il gating d'ampiezza leggeva il valore ADC prima della rimozione
// della DC, che porta l'offset dell'ADC. Ogni finestra veniva marcata come
// artefatto, congelando per sempre la velocità: lo zoom restava incollato e
// l'interazione non rispondeva. Il gating deve guardare il segnale DC-free.
console.log('\n[2b] Regressione: offset DC non deve marcare artefatti');
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 6, 30, 10, -1, 0, 6000);   // offset DC di ~531 µV + 30 µV
const falsePositives = emits.filter(e => e.artifact).length;
check('segnale pulito con offset DC realistico -> nessun artefatto',
      falsePositives === 0, `falsi positivi: ${falsePositives}/${emits.length}`);
check('ampiezza misurata coerente con il segnale, non con l\'offset',
      emits[emits.length - 1].maxAbsRaw < 100,
      `maxAbsRaw=${emits[emits.length - 1].maxAbsRaw.toFixed(1)} µV`);

// Con l'offset DC presente, un vero artefatto deve comunque essere rilevato.
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 6, 30, 10, 256 * 3, 400, 6000);
check('con offset DC un vero artefatto viene comunque rilevato',
      emits.some(e => e.artifact), `artefatti=${emits.filter(e => e.artifact).length}`);

// ---------------------------------------------------------------
// Gating RELATIVO: con elettrodi dry il picco su 1 s sta normalmente su centinaia
// di µV (misurato sul campo: 130-800 µV). Una soglia assoluta da letteratura
// marcava tutto come artefatto e il controllo non partiva mai.
console.log('\n[2c] Gating relativo: ampiezza alta ma stabile non è artefatto');
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 8, 300, 10);        // 300 µV stabili
const steady = emits.slice(10);
const steadyFlagged = steady.filter(e => e.artifact).length;
check('ampiezza alta ma stabile -> nessun artefatto a regime',
      steadyFlagged === 0, `marcate=${steadyFlagged}/${steady.length}`);

// Sulla stessa base, un picco molto più grande deve comunque essere rilevato.
// Nota: il fondo scala a 14 bit è ±725 µV, valori oltre wrappano - il picco di
// test deve stare dentro il range rappresentabile.
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 8, 200, 10, 256 * 5, 700);
check('su base alta un vero picco viene comunque rilevato',
      emits.some(e => e.artifact), `artefatti=${emits.filter(e => e.artifact).length}`);

// ---------------------------------------------------------------
console.log('\n[3] Proxy qualità contatto');
muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 4, 30, 10);
check('segnale sano -> contactOk = true', emits.every(e => e.contactOk),
      `ok=${emits.filter(e => e.contactOk).length}/${emits.length}`);

muse = new MuseBluetooth();
emits = runSignal(muse, 256 * 4, 0, 10);      // canale piatto = elettrodo staccato
check('canale piatto -> contactOk = false', emits.every(e => !e.contactOk),
      `bad=${emits.filter(e => !e.contactOk).length}/${emits.length}`);

// ---------------------------------------------------------------
console.log('\n[4] Contenuto spettrale (la finestra di Hann non rompe la FFT)');
muse = new MuseBluetooth();
runSignal(muse, 256 * 4, 40, 10);             // sinusoide a 10 Hz
const lastMags = muse.outFFTMags[1];          // buffer riusato: contiene l'ultima finestra

let peakBin = 0;
for (let i = 1; i < 60; i++) if (lastMags[i] > lastMags[peakBin]) peakBin = i;
check(`picco spettrale sul bin ~10 Hz (bin=${peakBin})`, Math.abs(peakBin - 10) <= 1);

console.log(`\n=== ${pass} passed, ${fail} failed ===`);
process.exit(fail > 0 ? 1 : 0);
