/*
  MuseBluetooth.js (Versione Ottimizzata High-Performance - Raw FFT Solo)
*/

class MuseBluetooth {
    constructor() {
        this.SERVICE_UUID = '0000fe8d-0000-1000-8000-00805f9b34fb';
        this.CONTROL_UUID = '273e0001-4c4d-454d-96be-f03bac821358';
        this.EEG_UUIDS = ['273e0013-4c4d-454d-96be-f03bac821358'];

        this.onEEGDataCallback = null;
        this.onRawDataCallback = null;
        this.onDisconnectCallback = null;
        this.onReconnectCallback = null;

        this.autoReconnect = true;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 8;
        // Un solo tentativo di riaggancio alla volta: senza questo guard, un evento
        // gattserverdisconnected che arriva mentre un retry è già in coda (o in corso)
        // accoda una SECONDA sessione. Due _establishSession concorrenti che chiamano
        // gatt.connect() sullo stesso device fanno fallire entrambi ("Connection attempt
        // failed") ed è esattamente il motivo per cui il riaggancio non riusciva mai.
        this._reconnectPending = false;
        this._sessionBusy = false;

        this.device = null;
        this.server = null;
        this.controlChar = null;
        this.eegChars = {};

        const CFG = (typeof window !== 'undefined' && window.CONFIG) ? window.CONFIG : {};

        this.FS = 256;
        this.BUFFER_SIZE = CFG.STFT_WINDOW || 256;

        // STFT a finestra scorrevole: la finestra resta di BUFFER_SIZE campioni,
        // ma la FFT viene rieseguita ogni STFT_HOP campioni -> update a ~FS/HOP Hz.
        this.STFT_HOP = CFG.STFT_HOP || 48;
        this.hopCounter = 0;
        this.totalSamples = 0;

        this.channelLabels = ['TP9','AF7','AF8','TP10'];
        this.channelCount = this.channelLabels.length;

        this.realBuffers = [
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE)
        ];

        // Ring paralleli con il segnale in µV dopo la sola rimozione della DC e prima
        // di notch/passa-basso: è questo il segnale su cui ha senso il gating d'ampiezza.
        // NON si può usare il valore ADC grezzo: _get14BitSigned interpreta come signed
        // valori che il Muse invia centrati su 8192, quindi il riposo vale ~-725 µV e
        // sfonderebbe qualunque soglia da letteratura in modo permanente.
        this.dcFreeBuffers = Array.from({length: this.channelCount}, () => new Float32Array(this.BUFFER_SIZE));

        // Valori ADC unsigned grezzi (0..16383): servono a diagnosticare il decode.
        this.adcBuffers = Array.from({length: this.channelCount}, () => new Float32Array(this.BUFFER_SIZE));

        this.decodeMode = CFG.EEG_DECODE_MODE || 'unsigned-centered';

        // Storico dei picchi per finestra: base del gating relativo agli artefatti.
        this.peakHistorySize = CFG.ARTIFACT_PEAK_HISTORY || 48;
        this.peakHistory = new Float32Array(this.peakHistorySize);
        this.peakScratch = new Float32Array(this.peakHistorySize);
        this.peakCount = 0;
        this.peakWriteIdx = 0;

        // Diagnostica del flusso: campioni/secondo realmente ricevuti dall'hardware.
        // Devono essere ~256; valori molto più bassi indicano che si stanno perdendo
        // campioni nel parsing o che il device non sta streammando a pieno rate.
        this._spsCount = 0;
        this._spsAt = 0;
        this._spsMeasured = 0;
        this._diagPacketLen = 0;
        this._diagSamplesPerPacket = 0;

        this.writeIndex = [0, 0, 0, 0];
        this.sampleCounts = [0, 0, 0, 0];

        this.dcPredictor = [0.0, 0.0, 0.0, 0.0];
        // Il predittore DC parte da 0 e impiegherebbe ~0.5 s a raggiungere l'offset
        // reale dell'ADC: in quel transitorio il segnale sembra enorme e il gating lo
        // scarterebbe. Si inizializza col primo campione visto, così converge subito.
        this.dcInitialized = [false, false, false, false];
        
        this.notch_x1 = [0.0, 0.0, 0.0, 0.0]; this.notch_x2 = [0.0, 0.0, 0.0, 0.0];
        this.notch_y1 = [0.0, 0.0, 0.0, 0.0]; this.notch_y2 = [0.0, 0.0, 0.0, 0.0];

        this.lp_x1 = [0.0, 0.0, 0.0, 0.0]; this.lp_x2 = [0.0, 0.0, 0.0, 0.0];
        this.lp_y1 = [0.0, 0.0, 0.0, 0.0]; this.lp_y2 = [0.0, 0.0, 0.0, 0.0];

        this.fftReals = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftImags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftMags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE/2));

        this.outFFTMags = Array.from({length: this.channelCount}, () => new Float32Array(this.BUFFER_SIZE / 2));
        this.outLastSamples = new Float32Array(this.channelCount);

        // Oggetto qualità riusato ad ogni emit (zero allocazioni per-frame).
        this.qualityPayload = {
            maxAbsRaw: 0,     // picco µV grezzo sui canali frontali
            artifact: false,  // true -> finestra da scartare (blink, serramento mascella, movimento)
            contactOk: true,  // proxy di qualità contatto (vedi _assessQuality)
            adcMin: 0,        // diagnostica decode: statistiche ADC unsigned su AF7
            adcMax: 0,
            adcMean: 0
        };

        this.brainDataPayload = {
            timestamp: 0,
            lastSamples: this.outLastSamples,
            quality: this.qualityPayload,
            fft: { mags: this.outFFTMags, bins: this.BUFFER_SIZE / 2, fs: this.FS }
        };

        // Finestra di Hann: necessaria con finestre sovrapposte per contenere lo spectral leakage.
        this.hannWindow = new Float32Array(this.BUFFER_SIZE);
        for (let i = 0; i < this.BUFFER_SIZE; i++) {
            this.hannWindow[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (this.BUFFER_SIZE - 1)));
        }

        this.fftLookUpTable = new Float32Array(this.BUFFER_SIZE * 2);
        const PI2 = -2 * Math.PI;
        for (let i = 0; i < this.BUFFER_SIZE; i++) {
            this.fftLookUpTable[i * 2] = Math.cos(i * (PI2 / this.BUFFER_SIZE));
            this.fftLookUpTable[i * 2 + 1] = Math.sin(i * (PI2 / this.BUFFER_SIZE));
        }

        this._lastRawAt = 0;
        this._lastFFTAt = 0;

        this._handleDisconnect = this._handleDisconnect.bind(this);
        this.handleIncomingEEGPacket = this.handleIncomingEEGPacket.bind(this);
    }

    onEEGData(cb){ this.onEEGDataCallback = cb; }
    onReconnect(cb){ this.onReconnectCallback = cb; }
    onRawData(cb){ this.onRawDataCallback = cb; }
    onDisconnect(cb){ this.onDisconnectCallback = cb; }
    _sleep(ms){ return new Promise(resolve => setTimeout(resolve, ms)); }
    async sendControlCommand(cmd){ return await this._sendCommand(cmd); }

    async connect(){
        console.log('Requesting Muse Device...');
        this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [this.SERVICE_UUID] }] });
        this.device.addEventListener('gattserverdisconnected', this._handleDisconnect);
        this.reconnectAttempts = 0;
        this._reconnectPending = false;
        this.autoReconnect = true;
        await this._establishSession();
    }

    /**
     * Apre la sessione GATT e configura lo streaming. Separato da connect() perché
     * la riconnessione deve poterlo rifare senza richiedere un nuovo gesto utente
     * (il device resta autorizzato finché l'oggetto BluetoothDevice è vivo).
     */
    async _establishSession(){
        // Reingresso: se una sessione si sta già aprendo, non aprirne una seconda in
        // parallelo (stomperebbe controlChar/eegChars e farebbe fallire il GATT).
        if(this._sessionBusy){ throw new Error('sessione già in apertura'); }
        this._sessionBusy = true;
        try{
            // Se il device risulta ancora connesso da un drop sporco, chiudi prima:
            // un gatt.connect() su una sessione fantasma fallisce su Windows/Chrome.
            if(this.device.gatt && this.device.gatt.connected){
                try{ this.device.gatt.disconnect(); }catch(e){}
                await this._sleep(200);
            }
            this.server = await this.device.gatt.connect();
            const service = await this.server.getPrimaryService(this.SERVICE_UUID);

            this.controlChar = await service.getCharacteristic(this.CONTROL_UUID);
            await this.controlChar.startNotifications();
            
            const uuid = this.EEG_UUIDS[0];
            const char = await service.getCharacteristic(uuid);
            await char.startNotifications();
            char.addEventListener('characteristicvaluechanged', (e) => this.handleIncomingEEGPacket(uuid, e.target.value));
            this.eegChars[uuid] = char;

            await this.sendControlCommand('v6'); await this._sleep(100);
            await this.sendControlCommand('s'); await this._sleep(100);
            await this.sendControlCommand('h'); await this._sleep(100);
            await this.sendControlCommand('p21'); await this._sleep(200);
            await this.sendControlCommand('dc001'); await this.sendControlCommand('L1'); await this._sleep(300);
            await this.sendControlCommand('h'); await this._sleep(100);
            await this.sendControlCommand('p1041'); await this._sleep(200);
            await this.sendControlCommand('dc001'); await this.sendControlCommand('L1'); await this._sleep(200);
            await this.sendControlCommand('s');
            console.log('[MuseBluetooth] Driver pronto ed ottimizzato.');
        }catch(err){
            // NON chiamare _handleDisconnect() qui: il chiamante (connect iniziale o il
            // retry schedulato) gestisce il fallimento una sola volta. Chiamarlo qui
            // schedulava un secondo riaggancio in parallelo a quello del setTimeout.
            console.error('Connection failed', err);
            throw err;
        }finally{
            this._sessionBusy = false;
        }
    }

    async disconnect(){
        this.autoReconnect = false;   // disconnessione voluta: non ritentare
        if(this.device && this.device.gatt && this.device.gatt.connected) this.device.gatt.disconnect();
    }

    /**
     * Il Muse cade da solo con una certa frequenza. Senza riaggancio automatico
     * l'esperienza finisce lì e l'utente deve rifare tutto il giro di pairing.
     */
    _handleDisconnect(){
        if(typeof this.onDisconnectCallback === 'function') this.onDisconnectCallback();

        if(!this.autoReconnect || !this.device) return;
        // Un riaggancio è già in coda o in corso: non accodarne un secondo. È questo
        // che evita i tentativi sovrapposti che si sabotavano a vicenda.
        if(this._reconnectPending || this._sessionBusy) return;
        if(this.reconnectAttempts >= this.maxReconnectAttempts){
            console.warn(`[MuseBluetooth] riaggancio esaurito dopo ${this.reconnectAttempts} tentativi`);
            return;
        }

        const attempt = ++this.reconnectAttempts;
        const delay = Math.min(500 * Math.pow(2, attempt - 1), 4000);   // backoff
        console.warn(`[MuseBluetooth] riaggancio automatico fra ${delay}ms (${attempt}/${this.maxReconnectAttempts})`);

        this._reconnectPending = true;
        setTimeout(async () => {
            this._reconnectPending = false;
            try{
                await this._establishSession();
                this.reconnectAttempts = 0;
                console.log('[MuseBluetooth] riagganciato');
                if(typeof this.onReconnectCallback === 'function') this.onReconnectCallback();
            }catch(e){
                console.warn('[MuseBluetooth] riaggancio fallito:', e.message || e);
                this._handleDisconnect();   // ritenta finché restano tentativi
            }
        }, delay);
    }

    async _sendCommand(cmd){
        if(!this.controlChar) return null;
        const encoder = new TextEncoder();
        const bytes = encoder.encode(cmd + '\n');
        const payload = new Uint8Array(bytes.length + 1);
        payload[0] = bytes.length; payload.set(bytes, 1);
        try{ await this.controlChar.writeValueWithoutResponse(payload); }catch(e){ try{ await this.controlChar.writeValue(payload); }catch(e2){ return null; } }
        return await this._awaitControlNotification(1000);
    }

    _awaitControlNotification(timeoutMs=1000){
        if(!this.controlChar) return Promise.resolve(null);
        return new Promise(resolve => {
            let finished = false;
            const handler = (e)=>{
                if(finished) return; finished = true;
                try{ const data = new Uint8Array(e.target.value.buffer, e.target.value.byteOffset, e.target.value.byteLength); this.controlChar.removeEventListener('characteristicvaluechanged', handler); resolve(data); }catch(err){ resolve(null); }
            };
            this.controlChar.addEventListener('characteristicvaluechanged', handler);
            setTimeout(()=>{ if(finished) return; finished = true; try{ this.controlChar.removeEventListener('characteristicvaluechanged', handler); }catch(e){}; resolve(null); }, timeoutMs);
        });
    }

    handleIncomingEEGPacket(uuid, value){
        try{
            let dataView = value;
            if(!(dataView && typeof dataView.getUint8 === 'function')){
                dataView = new DataView(new Uint8Array(value).buffer);
            }

            const len = dataView.byteLength || 0;
            if(len < 10) return;

            const packetIdByte = dataView.getUint8(9);
            const dataType = packetIdByte & 0x0F;
            if (dataType !== 1 && dataType !== 2) return; 

            if(typeof this.onRawDataCallback === 'function') {
                this.onRawDataCallback(new Uint8Array(dataView.buffer, dataView.byteOffset, len));
            }
            this._lastRawAt = Date.now();

            const headerOffset = 14; 
            if(len <= headerOffset) return;

            const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + headerOffset, len - headerOffset);
            const numChannels = (dataType === 2) ? 8 : 4;

            let bitOffset = 0;
            const totalBits = payload.length * 8;

            // Il numero di campioni per pacchetto NON è fisso: si ricava da quanti ne
            // stanno nel payload. Con un valore hardcoded a 2 si scartavano tutti i
            // campioni successivi, riducendo il rate effettivo a una frazione dei 256 Hz
            // dell'hardware (misurato sul campo: ~38 campioni/s, cioè 1/6).
            const numSamples = Math.floor(totalBits / (14 * numChannels));
            if (numSamples <= 0) return;

            this._diagPacketLen = len;
            this._diagSamplesPerPacket = numSamples;

            for (let s = 0; s < numSamples; s++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    if (bitOffset + 14 > totalBits) break;
                    const rawUnsigned = this._get14BitRaw(payload, bitOffset);
                    const rawVal = this._centerSample(rawUnsigned);
                    bitOffset += 14;

                    if (ch < 4) {
                        if (!this.dcPredictor || this.dcPredictor[ch] === undefined) continue;

                        const uv = rawVal * (1450.0 / 16383.0);

                        if (!this.dcInitialized[ch]) {
                            this.dcPredictor[ch] = uv;
                            this.dcInitialized[ch] = true;
                        }

                        let filtered = uv - this.dcPredictor[ch];
                        this.dcPredictor[ch] += 0.02 * filtered;
                        const dcFree = filtered;   // per il gating d'ampiezza

                        const n_b0 = 0.9391, n_b1 = -0.4024, n_b2 = 0.9391;
                        const n_a1 = -0.4024, n_a2 = 0.8782;
                        let x = filtered;
                        let y = (n_b0 * x) + (n_b1 * this.notch_x1[ch]) + (n_b2 * this.notch_x2[ch]) - (n_a1 * this.notch_y1[ch]) - (n_a2 * this.notch_y2[ch]);
                        this.notch_x2[ch] = this.notch_x1[ch]; this.notch_x1[ch] = x;
                        this.notch_y2[ch] = this.notch_y1[ch]; this.notch_y1[ch] = y;
                        filtered = y;

                        const lp_b0 = 0.2066, lp_b1 = 0.4132, lp_b2 = 0.2066;
                        const lp_a1 = -0.3695, lp_a2 = 0.1958;
                        x = filtered;
                        y = (lp_b0 * x) + (lp_b1 * this.lp_x1[ch]) + (lp_b2 * this.lp_x2[ch]) - (lp_a1 * this.lp_y1[ch]) - (lp_a2 * this.lp_y2[ch]);
                        this.lp_x2[ch] = this.lp_x1[ch]; this.lp_x1[ch] = x;
                        this.lp_y2[ch] = this.lp_y1[ch]; this.lp_y1[ch] = y;
                        filtered = y;

                        const idx = (this.writeIndex[ch] + 1) % this.BUFFER_SIZE;
                        this.writeIndex[ch] = idx;
                        this.realBuffers[ch][idx] = filtered;
                        this.dcFreeBuffers[ch][idx] = dcFree;
                        this.adcBuffers[ch][idx] = rawUnsigned;
                        this.sampleCounts[ch] = Math.min(this.sampleCounts[ch] + 1, this.BUFFER_SIZE);
                    }
                }

                // Un tick di hop per campione (non per canale): i 4 canali avanzano insieme.
                this.totalSamples++;
                this.hopCounter++;
                this._spsCount++;

                if(this.totalSamples >= this.BUFFER_SIZE && this.hopCounter >= this.STFT_HOP){
                    this.hopCounter = 0;
                    this._runFFTAndEmit();
                }
            }
        }catch(err){ console.error('[MuseBluetooth] Error', err); }
    }

    _runFFTAndEmit(){
        const N = this.BUFFER_SIZE;
        for(let ch=0; ch<this.channelCount; ch++){
            const real = this.fftReals[ch];
            const imag = this.fftImags[ch];
            const src = this.realBuffers[ch];
            const tail = this.writeIndex[ch];
            const head = (tail + 1) % N;
            for(let i=0; i<N; i++){
                real[i] = src[(head + i) % N] * this.hannWindow[i];
                imag[i] = 0.0;
            }
            this._computeRadix2FFT(real, imag);

            const mags = this.fftMags[ch];
            for(let i=0; i<N/2; i++) mags[i] = Math.sqrt(real[i]*real[i] + imag[i]*imag[i]) / N;
        }

        for (let ch = 0; ch < this.channelCount; ch++) {
            this.outFFTMags[ch].set(this.fftMags[ch]);
            this.outLastSamples[ch] = this.realBuffers[ch][this.writeIndex[ch]] || 0;
        }

        this._assessQuality();

        // Misura del sample rate reale su finestra di 1 s.
        const now = Date.now();
        if (this._spsAt === 0) {
            this._spsAt = now;
        } else if (now - this._spsAt >= 1000) {
            this._spsMeasured = this._spsCount * 1000 / (now - this._spsAt);
            this._spsCount = 0;
            this._spsAt = now;
        }
        this.qualityPayload.sps = this._spsMeasured;
        this.qualityPayload.packetLen = this._diagPacketLen;
        this.qualityPayload.samplesPerPacket = this._diagSamplesPerPacket;

        this.brainDataPayload.timestamp = now;

        if(typeof this.onEEGDataCallback === 'function'){
            this.onEEGDataCallback(this.brainDataPayload);
        }
    }

    /**
     * Valuta la finestra corrente e popola this.qualityPayload.
     *
     * - artifact: gating d'ampiezza sul segnale dei canali frontali (AF7/AF8) dopo la
     *   rimozione della DC ma prima del filtraggio in banda. Blink e serramento mascella
     *   sforano ampiamente ARTIFACT_UV_RAW.
     * - contactOk: PROXY di qualità contatto basato sulla deviazione standard del
     *   segnale filtrato. NON è l'Horseshoe Indicator del Muse: leggere l'HSI reale
     *   richiede di sottoscrivere una characteristic dedicata (non ancora implementata).
     *   Un canale piatto (std ~0) indica elettrodo staccato; uno std enorme indica
     *   contatto instabile. Entrambi i casi -> freeze del controllo.
     */
    _assessQuality(){
        const CFG = (typeof window !== 'undefined' && window.CONFIG) ? window.CONFIG : {};
        const relMult = CFG.ARTIFACT_REL_MULT || 2.5;
        const uvFloor = (CFG.ARTIFACT_UV_FLOOR !== undefined) ? CFG.ARTIFACT_UV_FLOOR : 150;
        const stdMin = (CFG.CONTACT_STD_MIN !== undefined) ? CFG.CONTACT_STD_MIN : 0.5;

        const N = this.BUFFER_SIZE;
        const frontal = [1, 2]; // AF7, AF8

        let maxAbsRaw = 0;
        let contactOk = true;

        for(let k = 0; k < frontal.length; k++){
            const ch = frontal[k];
            const raw = this.dcFreeBuffers[ch];
            const filt = this.realBuffers[ch];

            let sum = 0;
            let sumSq = 0;
            for(let i = 0; i < N; i++){
                const a = raw[i] < 0 ? -raw[i] : raw[i];
                if(a > maxAbsRaw) maxAbsRaw = a;
                const f = filt[i];
                sum += f;
                sumSq += f * f;
            }

            const mean = sum / N;
            const variance = (sumSq / N) - (mean * mean);
            const std = Math.sqrt(variance > 0 ? variance : 0);
            // Solo il canale piatto è diagnosticabile in assoluto: significa elettrodo
            // staccato. Un limite superiore fisso boccerebbe un contatto dry normale.
            if(std < stdMin) contactOk = false;
        }

        // Gating relativo: artefatto se il picco sfonda rispetto ai picchi recenti.
        const medianPeak = this._medianPeak();
        const artifact = (this.peakCount >= 8) &&
                         (maxAbsRaw > uvFloor) &&
                         (maxAbsRaw > relMult * medianPeak);

        this.peakHistory[this.peakWriteIdx] = maxAbsRaw;
        this.peakWriteIdx = (this.peakWriteIdx + 1) % this.peakHistorySize;
        if(this.peakCount < this.peakHistorySize) this.peakCount++;

        this.qualityPayload.maxAbsRaw = maxAbsRaw;
        this.qualityPayload.medianPeak = medianPeak;
        this.qualityPayload.artifact = artifact;
        this.qualityPayload.contactOk = contactOk;

        // Statistiche ADC su AF7: dicono se il decode è corretto.
        // Valori stretti attorno a ~8192 -> l'ADC è unsigned centrato (decode ok).
        // Valori sparsi su tutto 0..16383 -> disallineamento dei bit / layout pacchetto.
        const adc = this.adcBuffers[1];
        let mn = Infinity, mx = -Infinity, sum = 0;
        for(let i = 0; i < N; i++){
            const v = adc[i];
            if(v < mn) mn = v;
            if(v > mx) mx = v;
            sum += v;
        }
        this.qualityPayload.adcMin = mn;
        this.qualityPayload.adcMax = mx;
        this.qualityPayload.adcMean = sum / N;
    }

    /** Mediana dei picchi recenti: riferimento per il gating relativo. */
    _medianPeak(){
        if(this.peakCount === 0) return 0;
        const s = this.peakScratch.subarray(0, this.peakCount);
        s.set(this.peakHistory.subarray(0, this.peakCount));
        s.sort();
        return s[this.peakCount >> 1];
    }

    /** Estrae il campione a 14 bit come intero UNSIGNED (0..16383). */
    _get14BitRaw(payload, bitOffset){
        const byteOffset = bitOffset >> 3;
        const bitShift = bitOffset & 7;
        const b0 = payload[byteOffset] || 0;
        const b1 = payload[byteOffset + 1] || 0;
        const b2 = payload[byteOffset + 2] || 0;
        return ((b0 | (b1 << 8) | (b2 << 16)) >> bitShift) & 0x3FFF;
    }

    /**
     * Converte il campione unsigned in valore centrato sullo zero.
     *
     * 'unsigned-centered' (default): l'ADC emette valori unsigned centrati sul
     *   fondo scala / 2, come nel decode di riferimento del Muse (che a 12 bit
     *   sottrae 0x800). A 14 bit il centro è 8192.
     * 'signed': interpretazione in complemento a due (comportamento originale).
     *   Se i valori reali oscillano attorno a 8192 questa produce un'onda quadra
     *   da ±725 µV invece che EEG, perché il segno si ribalta ad ogni attraversamento.
     */
    _centerSample(raw){
        if(this.decodeMode === 'signed'){
            return (raw > 8191) ? raw - 16384 : raw;
        }
        return raw - 8192;
    }

    _computeRadix2FFT(real, imag){
        const n = real.length;
        let j = 0;
        for(let i=0;i<n-1;i++){
            if(i < j){
                let tr = real[i]; let ti = imag[i]; real[i]=real[j]; imag[i]=imag[j]; real[j]=tr; imag[j]=ti;
            }
            let k = n >> 1;
            while(k <= j){ j -= k; k >>= 1; }
            j += k;
        }
        
        for(let size=2; size<=n; size<<=1){
            let half = size >> 1;
            for(let m=0;m<half;m++){
                const lutIdx = (m * (this.BUFFER_SIZE / size)) * 2;
                const wReal = this.fftLookUpTable[lutIdx];
                const wImag = this.fftLookUpTable[lutIdx + 1];
                
                for(let i=m;i<n;i+=size){
                    const l = i + half;
                    const tReal = wReal * real[l] - wImag * imag[l];
                    const tImag = wReal * imag[l] + wImag * real[l];
                    real[l] = real[i] - tReal; imag[l] = imag[i] - tImag;
                    real[i] += tReal; imag[i] += tImag;
                }
            }
        }
    }
}

window.MuseBluetooth = MuseBluetooth;