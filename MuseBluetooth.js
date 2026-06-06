/*
  MuseBluetooth.js
  Rewritten driver to support Muse S / Athena distribution of EEG across 4 characteristics.
  - Subscribes to 4 EEG characteristic UUIDs (TP9, AF7, AF8, TP10)
  - Demultiplexes incoming 14-bit packed samples per-characteristic
  - Maintains per-channel circular buffers and computes per-channel FFT
  - Emits aggregated Alpha/Beta/Focus metrics via `onEEGData(callback)`
  - Emits raw notification bytes via `onRawData(callback)` for debugging

  Notes:
  - Must be used from a secure context (HTTPS or localhost) with a browser supporting Web Bluetooth.
  - The parsing assumes each EEG notification contains a 2-byte packet counter followed by 14-bit packed samples.
    The parser reads as many 14-bit signed samples as fit into the packet payload.
*/

class MuseBluetooth {
    constructor() {
        // Service / Control / EEG UUIDs (fixed mapping)
        this.SERVICE_UUID = '0000fe8d-0000-1000-8000-00805f9b34fb';
        this.CONTROL_UUID = '273e0001-4c4d-454d-96be-f03bac821358';
        // Per Athena (Muse S), tutto il flusso dati è multiplexato sulla sola caratteristica 0013
        this.EEG_UUIDS = [
            '273e0013-4c4d-454d-96be-f03bac821358'
        ];

        // Public callbacks
        this.onEEGDataCallback = null; // function(brainData)
        this.onRawDataCallback = null; // function(Uint8Array)
        this.onDisconnectCallback = null;

        // GATT objects
        this.device = null;
        this.server = null;
        this.controlChar = null;
        this.eegChars = {}; // uuid -> characteristic

        // Processing constants and buffers
        this.FS = 256;
        this.BUFFER_SIZE = 256; // circular buffer length for time-domain
        this.MAX_UV = 50;
        this.FOCUS_SCALE = 150;

        // Per-channel buffers and FFT storage (4 logical channels: TP9, AF7, AF8, TP10)
        this.channelLabels = ['TP9','AF7','AF8','TP10'];
        this.channelCount = this.channelLabels.length;
        
        // Inizializzazione dei buffer reali per l'EEG
        this.realBuffers = [
            new Float32Array(this.BUFFER_SIZE), // Ch 0: TP9
            new Float32Array(this.BUFFER_SIZE), // Ch 1: AF7
            new Float32Array(this.BUFFER_SIZE), // Ch 2: AF8
            new Float32Array(this.BUFFER_SIZE)  // Ch 3: TP10
        ];

        // Inizializzazione degli indici di scrittura e conteggio campioni
        this.writeIndex = [0, 0, 0, 0];
        this.sampleCounts = [0, 0, 0, 0];

        // Stato dei filtri per i 4 canali principali
        this.dcPredictor = [0.0, 0.0, 0.0, 0.0];
        
        // Memoria per il filtro Notch 50Hz (quattro zeri stabili)
        this.notch_x1 = [0.0, 0.0, 0.0, 0.0]; 
        this.notch_x2 = [0.0, 0.0, 0.0, 0.0];
        this.notch_y1 = [0.0, 0.0, 0.0, 0.0]; 
        this.notch_y2 = [0.0, 0.0, 0.0, 0.0];

        // Memoria per il filtro Passa-Basso 45Hz
        this.lp_x1 = [0.0, 0.0, 0.0, 0.0]; 
        this.lp_x2 = [0.0, 0.0, 0.0, 0.0];
        this.lp_y1 = [0.0, 0.0, 0.0, 0.0]; 
        this.lp_y2 = [0.0, 0.0, 0.0, 0.0];

        this.fftReals = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftImags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftMags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE/2));

        this.focusEMA = 0;
        this._lastRawAt = 0;
        this._lastFFTAt = 0;

        // Default mapping for Athena (3 characteristics -> 4 logical EEG channels)
        this.DEFAULT_CHAR_TO_CHANNELS = {
            '273e0013-4c4d-454d-96be-f03bac821358': [0],    // TP9
            '273e0014-4c4d-454d-96be-f03bac821358': [1],    // AF7
            '273e0015-4c4d-454d-96be-f03bac821358': [2,3]   // AF8 (2), TP10 (3) interleaved
        };

        // Fallback mapping (lowercase uuid -> {channels: [idx,...], interleaved: bool})
        this.charToChannelMap = {};
        if(this.EEG_UUIDS && this.EEG_UUIDS.length > 0) this.charToChannelMap[this.EEG_UUIDS[0].toLowerCase()] = { channels: [0], interleaved: false };
        if(this.EEG_UUIDS && this.EEG_UUIDS.length > 1) this.charToChannelMap[this.EEG_UUIDS[1].toLowerCase()] = { channels: [1], interleaved: false };
        if(this.EEG_UUIDS && this.EEG_UUIDS.length > 2) this.charToChannelMap[this.EEG_UUIDS[2].toLowerCase()] = { channels: [2,3], interleaved: true };

        // Bindings
        this._handleDisconnect = this._handleDisconnect.bind(this);
        this.handleIncomingEEGPacket = this.handleIncomingEEGPacket.bind(this);
    }

    // Public registration helpers
    onEEGData(cb){ this.onEEGDataCallback = cb; }
    onRawData(cb){ this.onRawDataCallback = cb; }
    onDisconnect(cb){ this.onDisconnectCallback = cb; }

    // Helper sleep used during handshake delays
    _sleep(ms){ return new Promise(resolve => setTimeout(resolve, ms)); }

    // Public wrapper name expected elsewhere
    async sendControlCommand(cmd){ return await this._sendCommand(cmd); }

    // Connect to device and subscribe to control + all EEG characteristics
    async connect(){
        try{
            console.log('Requesting Muse Bluetooth Device...');
            this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [this.SERVICE_UUID] }] });
            this.device.addEventListener('gattserverdisconnected', this._handleDisconnect);

            console.log('Connecting to GATT Server...');
            this.server = await this.device.gatt.connect();

            console.log('Getting Service...');
            const service = await this.server.getPrimaryService(this.SERVICE_UUID);

            console.log('Getting Control Characteristic...');
            this.controlChar = await service.getCharacteristic(this.CONTROL_UUID);
            await this.controlChar.startNotifications();
            this.controlChar.addEventListener('characteristicvaluechanged', (e)=>{
                try{
                    const dv = e.target.value;
                    const raw = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
                    const preview = Array.from(raw.slice(0,32)).map(b=>b.toString(16).padStart(2,'0')).join(' ');
                    console.log('[MuseBluetooth] Control notify', dv.byteLength, 'bytes:', preview);
                }catch(err){ console.warn('[MuseBluetooth] Control notify parse failed', err); }
            });
            console.log('Control notifications started', this.CONTROL_UUID);

            // Sottoscrizione all'unica caratteristica multiplexata effettiva di Athena
            const uuid = this.EEG_UUIDS[0];
            this.eegChars = {};
            try {
                const char = await service.getCharacteristic(uuid);
                await char.startNotifications();
                char.addEventListener('characteristicvaluechanged', (e) => this.handleIncomingEEGPacket(uuid, e.target.value));
                this.eegChars[uuid] = char;
                console.log('[MuseBluetooth] Multiplexed Athena notification started', uuid);
            } catch(err) {
                console.error('[MuseBluetooth] Impossibile connettersi alla caratteristica dati 0013:', err);
                throw err;
            }

            // --- SEQUENZA DOPPIO PRESET ATHENA CRITICA ---
            console.log('[MuseBluetooth] Starting Athena Handshake...');
            await this.sendControlCommand('v6'); // Identificazione corretta v6 per Athena
            await this._sleep(100);
            await this.sendControlCommand('s');
            await this._sleep(100);
            await this.sendControlCommand('h');
            await this._sleep(100);

            // 1. Applichiamo il preset base EEG
            await this.sendControlCommand('p21');
            await this._sleep(200);

            // 2. Primo innesco per svegliare i descrittori hardware
            await this.sendControlCommand('dc001');
            await this.sendControlCommand('L1');
            await this._sleep(300);

            // 3. Mettiamo temporaneamente in pausa
            await this.sendControlCommand('h');
            await this._sleep(100);

            // 4. Applichiamo il preset finale completo (p1041 abilita modalità EEG8 a 8 canali)
            await this.sendControlCommand('p1041');
            await this._sleep(200);

            // 5. Secondo innesco definitivo obbligatorio
            await this.sendControlCommand('dc001');
            await this.sendControlCommand('L1');
            await this._sleep(200);

            // 6. Avvio definitivo dello streaming
            await this.sendControlCommand('s');
            console.log('[MuseBluetooth] Stream activated via dual-preset sequence.');

            console.log('Muse connected and streaming!');
        }catch(err){
            console.error('Connection failed', err);
            this._handleDisconnect();
            throw err;
        }
    }

    async disconnect(){ if(this.device && this.device.gatt && this.device.gatt.connected) this.device.gatt.disconnect(); }

    _handleDisconnect(){ console.log('[MuseBluetooth] Muse disconnected'); if(typeof this.onDisconnectCallback === 'function') this.onDisconnectCallback(); }

    // Send control command (OpenMuse style: length-prefixed string + '\n')
    async _sendCommand(cmd){
        if(!this.controlChar) return null;
        const encoder = new TextEncoder();
        const str = cmd + '\n';
        const bytes = encoder.encode(str);
        const payload = new Uint8Array(bytes.length + 1);
        payload[0] = bytes.length;
        payload.set(bytes, 1);

        try{
            await this.controlChar.writeValueWithoutResponse(payload);
            console.log('[MuseBluetooth] Sent control command', cmd, 'payloadLen=', payload.length);
        }catch(e){
            console.warn('[MuseBluetooth] writeValueWithoutResponse failed, trying writeValue', e);
            try{ await this.controlChar.writeValue(payload); console.log('[MuseBluetooth] Sent control command (writeValue)', cmd); }catch(e2){ console.warn('[MuseBluetooth] control write failed', e2); }
        }

        // Wait shortly for any control notifications to appear so handshake is visible
        try{
            const resp = await this._awaitControlNotification(1500);
            if(resp){
                const preview = Array.from(resp.slice(0,64)).map(b=>b.toString(16).padStart(2,'0')).join(' ');
                let ascii=''; try{ ascii = new TextDecoder().decode(resp); }catch(e){}
                console.log('[MuseBluetooth] Control response for', cmd, 'len=', resp.length, 'hex=', preview, ascii?('ascii="'+ascii+'"'):'');
            }else{
                console.log('[MuseBluetooth] No control response for', cmd, 'within timeout');
            }
            return resp;
        }catch(e){ console.warn('[MuseBluetooth] awaiting control response failed', e); return null; }
    }

    _awaitControlNotification(timeoutMs=1500){
        if(!this.controlChar) return Promise.resolve(null);
        return new Promise(resolve => {
            let finished = false;
            const handler = (e)=>{
                if(finished) return;
                finished = true;
                try{ const dv = e.target.value; const data = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength); this.controlChar.removeEventListener('characteristicvaluechanged', handler); resolve(data); }catch(err){ this.controlChar.removeEventListener('characteristicvaluechanged', handler); resolve(null); }
            };
            this.controlChar.addEventListener('characteristicvaluechanged', handler);
            setTimeout(()=>{ if(finished) return; finished = true; try{ this.controlChar.removeEventListener('characteristicvaluechanged', handler); }catch(e){}; resolve(null); }, timeoutMs);
        });
    }

    // Parse incoming EEG notification coming from a particular characteristic UUID
    handleIncomingEEGPacket(uuid, value){
        try{
            // Normalizzazione del DataView in ingresso (gestisce sia ArrayBuffer che DataView)
            let dataView = value;
            if(!(dataView && typeof dataView.getUint8 === 'function')){
                const arr = new Uint8Array(value);
                dataView = new DataView(arr.buffer);
            }

            const len = dataView.byteLength || 0;
            if(len < 10) return; // Pacchetto troppo corto per contenere l'header esteso Athena

            // --- PARSING STRUTTURA PACCHETTO ATHENA ---
            // Struttura standard Athena: <len(1B)><counter(1B)><unknown(7B)><packet_id_byte(1B)>
            const packetIdByte = dataView.getUint8(9);
            const dataType = packetIdByte & 0x0F; // Isola gli ultimi 4 bit (il TAG del tipo dati)

            // Filtro dei tipi dati: 1 = EEG 4 canali (preset p21), 2 = EEG 8 canali (preset p1041)
            if (dataType !== 1 && dataType !== 2) {
                // Se è un pacchetto IMU (7), Ottico/fNIRS (4/5/6) o Batteria (8), lo ignoriamo
                return; 
            }

            // Emetti i byte grezzi per la visualizzazione di debug se la callback è attiva
            if(typeof this.onRawDataCallback === 'function') {
                const raw = new Uint8Array(dataView.buffer, dataView.byteOffset, len);
                this.onRawDataCallback(raw);
            }
            this._lastRawAt = Date.now();

            // Il payload compresso a 14-bit inizia dopo l'header esteso e i contatori di sotto-pacchetto
            const headerOffset = 14; 
            if(len <= headerOffset) return;

            const payloadLen = len - headerOffset;
            const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + headerOffset, payloadLen);

            // Configurazione dinamica dei canali in base al tipo di pacchetto EEG rilevato
            const numChannels = (dataType === 2) ? 8 : 4; 
            const numSamples = 2;  // Ogni sotto-pacchetto Athena contiene sempre 2 campioni temporali consecutivi

            let bitOffset = 0;
            const totalBits = payload.length * 8;

            // Ciclo sui campioni e sui canali multiplexati
            for (let s = 0; s < numSamples; s++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    // Controllo di sicurezza per non sforare i bit disponibili nel payload
                    if (bitOffset + 14 > totalBits) break;

                    // Estrazione del valore signed a 14 bit tramite la funzione helper interna
                    const rawVal = this._get14BitSigned(payload, bitOffset);
                    bitOffset += 14;

                    // SEPARAZIONE DEI CANALI: Processiamo e filtriamo solo i primi 4 canali utili alla UI
                    if (ch < 4) {
                        // Verifica di sicurezza sull'inizializzazione degli array di stato dei filtri
                        if (!this.dcPredictor || this.dcPredictor[ch] === undefined) continue;

                        // Conversione matematica da valori interi grezzi a microvolt nativi
                        const uv = rawVal * (1450.0 / 16383.0);

                        // --- 1. FILTRO CC (PASSA-ALTO ~0.5Hz) ---
                        let filtered = uv - this.dcPredictor[ch];
                        this.dcPredictor[ch] += 0.02 * filtered;

                        // --- 2. FILTRO NOTCH 50Hz (Fs=256Hz, Q=10) ---
                        const n_b0 = 0.9391, n_b1 = -0.4024, n_b2 = 0.9391;
                        const n_a1 = -0.4024, n_a2 = 0.8782;
                        
                        let x = filtered;
                        let y = (n_b0 * x) + (n_b1 * this.notch_x1[ch]) + (n_b2 * this.notch_x2[ch])
                                - (n_a1 * this.notch_y1[ch]) - (n_a2 * this.notch_y2[ch]);
                        
                        this.notch_x2[ch] = this.notch_x1[ch]; this.notch_x1[ch] = x;
                        this.notch_y2[ch] = this.notch_y1[ch]; this.notch_y1[ch] = y;
                        filtered = y;

                        // --- 3. FILTRO PASSA-BASSO 45Hz (Butterworth 2° Ordine, Fs=256Hz) ---
                        const lp_b0 = 0.2066, lp_b1 = 0.4132, lp_b2 = 0.2066;
                        const lp_a1 = -0.3695, lp_a2 = 0.1958;
                        
                        x = filtered;
                        y = (lp_b0 * x) + (lp_b1 * this.lp_x1[ch]) + (lp_b2 * this.lp_x2[ch])
                            - (lp_a1 * this.lp_y1[ch]) - (lp_a2 * this.lp_y2[ch]);
                        
                        this.lp_x2[ch] = this.lp_x1[ch]; this.lp_x1[ch] = x;
                        this.lp_y2[ch] = this.lp_y1[ch]; this.lp_y1[ch] = y;
                        filtered = y;

                        // --- 4. SOGLIA DI RIGETTO ARTEFATTI (Blink oculari) ---
                        if ((ch === 1 || ch === 2) && Math.abs(filtered) > 150) {
                            filtered = 0.0; 
                        }

                        // --- 5. SCRITTURA NEI BUFFER CIRCOLARI ---
                        const idx = (this.writeIndex[ch] + 1) % this.BUFFER_SIZE;
                        this.writeIndex[ch] = idx;
                        
                        this.realBuffers[ch][idx] = filtered; 
                        this.sampleCounts[ch] = Math.min(this.sampleCounts[ch] + 1, this.BUFFER_SIZE);
                    }
                }
            }

            // --- TRIGGER DI CALCOLO DELLA FFT ---
            if(this.sampleCounts[0] >= this.BUFFER_SIZE){
                this._runFFTAndEmit();
                
                // RESET DEI CONTEGGI: Previene cicli infiniti bloccanti riavviando l'accumulo sequenziale
                for(let i = 0; i < this.channelCount; i++){
                    this.sampleCounts[i] = 0;
                }
            }

        }catch(err){ 
            console.error('[MuseBluetooth] handleIncomingEEGPacket failed', err); 
        }
    }

    // Compute per-channel FFTs using the circular buffers and emit aggregated metrics
    _runFFTAndEmit(){
        const N = this.BUFFER_SIZE;
        // CORREZIONE: Scorre tutti i 4 canali allocati fisicamente, non la lunghezza dell'array UUIDs (che è 1)
        for(let ch=0; ch<this.channelCount; ch++){
            const real = this.fftReals[ch];
            const imag = this.fftImags[ch];
            const src = this.realBuffers[ch];
            const tail = this.writeIndex[ch];
            const head = (tail + 1) % N; // oldest sample
            for(let i=0; i<N; i++){
                real[i] = src[(head + i) % N];
                imag[i] = 0.0;
            }
            this._computeRadix2FFT(real, imag);

            // Fill mags
            const mags = this.fftMags[ch];
            for(let i=0; i<N/2; i++) mags[i] = Math.sqrt(real[i]*real[i] + imag[i]*imag[i]) / N;
        }

        // Aggregate Alpha (8-12) and Beta (13-30) across all 4 channels
        let alphaSum = 0, betaSum = 0;
        for(let ch=0; ch<this.channelCount; ch++){
            const mags = this.fftMags[ch];
            for(let b=8; b<=12; b++) alphaSum += mags[b] || 0;
            for(let b=13; b<=30; b++) betaSum += mags[b] || 0;
        }
        const alphaMean = alphaSum / (5 * this.channelCount);
        const betaMean = betaSum / (18 * this.channelCount);

        const alphaAmp = alphaMean;
        const betaAmp = betaMean;
        const focusRatio = betaAmp / (alphaAmp + 0.001);
        let newFocus = focusRatio * this.FOCUS_SCALE;
        newFocus = Math.max(0, Math.min(255, newFocus));
        this.focusEMA = this.focusEMA * 0.8 + newFocus * 0.2;

        const alphaPct = Math.min(100, (alphaAmp / this.MAX_UV) * 100);
        const betaPct = Math.min(100, (betaAmp / this.MAX_UV) * 100);

        // Last samples per channel (most recent) - corretto mapping su channelCount
        const lastSamples = Array.from({length: this.channelCount}, (_, ch) => this.realBuffers[ch][this.writeIndex[ch]] || 0);

        // Build FFT copy to include in callback (shallow copy per channel)
        const fftCopy = this.fftMags.map(m => new Float32Array(m));

        const brainData = {
            timestamp: Date.now(),
            focus: Math.round(this.focusEMA),
            alpha: alphaPct,
            beta: betaPct,
            lastSamples: lastSamples,
            fft: { mags: fftCopy, bins: N/2, fs: this.FS }
        };

        if(typeof this.onEEGDataCallback === 'function'){
            try{ this.onEEGDataCallback(brainData); }catch(e){ console.error('[MuseBluetooth] onEEGDataCallback error', e); }
        }
    }

    // Unpacks 14-bit signed integer from continuous bit stream (payload: Uint8Array)
    _get14BitSigned(payload, bitOffset){
        const byteOffset = bitOffset >> 3;
        const bitShift = bitOffset & 7;
        
        // Estrazione di 3 byte consecutivi per coprire la finestra a 14 bit cross-byte
        const b0 = payload[byteOffset] || 0;
        const b1 = payload[byteOffset + 1] || 0;
        const b2 = payload[byteOffset + 2] || 0;

        // Costruzione dell'intero a 24-bit (Little-Endian a livello di byte)
        let val = b0 | (b1 << 8) | (b2 << 16);
        
        // Allineamento dello shift e maschera a 14 bit (0x3FFF)
        val = (val >> bitShift) & 0x3FFF;
        
        // Conversione in segno tramite Complemento a 2
        if(val > 8191) val -= 16384; 
        return val;
    }

    // In-place radix-2 FFT (real, imag are Float32Array of length power-of-two)
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
        const PI2 = -2 * Math.PI;
        for(let size=2; size<=n; size<<=1){
            let half = size >> 1;
            let step = PI2 / size;
            for(let m=0;m<half;m++){
                const wReal = Math.cos(m * step);
                const wImag = Math.sin(m * step);
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

// Expose class to global scope for simple inclusion via <script src="MuseBluetooth.js"></script>
window.MuseBluetooth = MuseBluetooth;