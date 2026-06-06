/*
  MuseBluetooth.js (Versione Ottimizzata High-Performance)
*/

class MuseBluetooth {
    constructor() {
        this.SERVICE_UUID = '0000fe8d-0000-1000-8000-00805f9b34fb';
        this.CONTROL_UUID = '273e0001-4c4d-454d-96be-f03bac821358';
        this.EEG_UUIDS = ['273e0013-4c4d-454d-96be-f03bac821358'];

        this.onEEGDataCallback = null;
        this.onRawDataCallback = null;
        this.onDisconnectCallback = null;

        this.device = null;
        this.server = null;
        this.controlChar = null;
        this.eegChars = {};

        this.FS = 256;
        this.BUFFER_SIZE = 256; 
        this.MAX_UV = 50;
        this.FOCUS_SCALE = 150;

        this.channelLabels = ['TP9','AF7','AF8','TP10'];
        this.channelCount = this.channelLabels.length;
        
        this.realBuffers = [
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE),
            new Float32Array(this.BUFFER_SIZE)
        ];

        this.writeIndex = [0, 0, 0, 0];
        this.sampleCounts = [0, 0, 0, 0];

        this.dcPredictor = [0.0, 0.0, 0.0, 0.0];
        
        this.notch_x1 = [0.0, 0.0, 0.0, 0.0]; this.notch_x2 = [0.0, 0.0, 0.0, 0.0];
        this.notch_y1 = [0.0, 0.0, 0.0, 0.0]; this.notch_y2 = [0.0, 0.0, 0.0, 0.0];

        this.lp_x1 = [0.0, 0.0, 0.0, 0.0]; this.lp_x2 = [0.0, 0.0, 0.0, 0.0];
        this.lp_y1 = [0.0, 0.0, 0.0, 0.0]; this.lp_y2 = [0.0, 0.0, 0.0, 0.0];

        this.fftReals = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftImags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.fftMags = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE/2));

        // --- OPTIMIZATION 1: MEMORY POOLING (Zero allocazioni a runtime) ---
        this.outFFTMags = Array.from({length: this.channelCount}, () => new Float32Array(this.BUFFER_SIZE / 2));
        this.outLastSamples = new Float32Array(this.channelCount);
        this.brainDataPayload = {
            timestamp: 0,
            focus: 0,
            alpha: 0,
            beta: 0,
            lastSamples: this.outLastSamples,
            fft: { mags: this.outFFTMags, bins: this.BUFFER_SIZE / 2, fs: this.FS }
        };

        // --- OPTIMIZATION 2: LOOK-UP TABLE TRIGONOMETRICA PER FFT ---
        this.fftLookUpTable = new Float32Array(this.BUFFER_SIZE * 2);
        const PI2 = -2 * Math.PI;
        for (let i = 0; i < this.BUFFER_SIZE; i++) {
            this.fftLookUpTable[i * 2] = Math.cos(i * (PI2 / this.BUFFER_SIZE));
            this.fftLookUpTable[i * 2 + 1] = Math.sin(i * (PI2 / this.BUFFER_SIZE));
        }

        this.focusEMA = 0;
        this._lastRawAt = 0;
        this._lastFFTAt = 0;

        this._handleDisconnect = this._handleDisconnect.bind(this);
        this.handleIncomingEEGPacket = this.handleIncomingEEGPacket.bind(this);
    }

    onEEGData(cb){ this.onEEGDataCallback = cb; }
    onRawData(cb){ this.onRawDataCallback = cb; }
    onDisconnect(cb){ this.onDisconnectCallback = cb; }
    _sleep(ms){ return new Promise(resolve => setTimeout(resolve, ms)); }
    async sendControlCommand(cmd){ return await this._sendCommand(cmd); }

    async connect(){
        try{
            console.log('Requesting Muse Device...');
            this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [this.SERVICE_UUID] }] });
            this.device.addEventListener('gattserverdisconnected', this._handleDisconnect);
            this.server = await this.device.gatt.connect();
            const service = await this.server.getPrimaryService(this.SERVICE_UUID);

            this.controlChar = await service.getCharacteristic(this.CONTROL_UUID);
            await this.controlChar.startNotifications();
            
            const uuid = this.EEG_UUIDS[0];
            const char = await service.getCharacteristic(uuid);
            await char.startNotifications();
            char.addEventListener('characteristicvaluechanged', (e) => this.handleIncomingEEGPacket(uuid, e.target.value));
            this.eegChars[uuid] = char;

            // Handshake sequenza doppio preset Athena obbligatoria
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
            console.error('Connection failed', err);
            this._handleDisconnect();
            throw err;
        }
    }

    async disconnect(){ if(this.device && this.device.gatt && this.device.gatt.connected) this.device.gatt.disconnect(); }
    _handleDisconnect(){ if(typeof this.onDisconnectCallback === 'function') this.onDisconnectCallback(); }

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
            const numSamples = 2;  

            let bitOffset = 0;
            const totalBits = payload.length * 8;

            for (let s = 0; s < numSamples; s++) {
                for (let ch = 0; ch < numChannels; ch++) {
                    if (bitOffset + 14 > totalBits) break;
                    const rawVal = this._get14BitSigned(payload, bitOffset);
                    bitOffset += 14;

                    if (ch < 4) {
                        if (!this.dcPredictor || this.dcPredictor[ch] === undefined) continue;

                        const uv = rawVal * (1450.0 / 16383.0);

                        // 1. Filtro CC
                        let filtered = uv - this.dcPredictor[ch];
                        this.dcPredictor[ch] += 0.02 * filtered;

                        // 2. Notch 50Hz
                        const n_b0 = 0.9391, n_b1 = -0.4024, n_b2 = 0.9391;
                        const n_a1 = -0.4024, n_a2 = 0.8782;
                        let x = filtered;
                        let y = (n_b0 * x) + (n_b1 * this.notch_x1[ch]) + (n_b2 * this.notch_x2[ch]) - (n_a1 * this.notch_y1[ch]) - (n_a2 * this.notch_y2[ch]);
                        this.notch_x2[ch] = this.notch_x1[ch]; this.notch_x1[ch] = x;
                        this.notch_y2[ch] = this.notch_y1[ch]; this.notch_y1[ch] = y;
                        filtered = y;

                        // 3. Passa-Basso 45Hz
                        const lp_b0 = 0.2066, lp_b1 = 0.4132, lp_b2 = 0.2066;
                        const lp_a1 = -0.3695, lp_a2 = 0.1958;
                        x = filtered;
                        y = (lp_b0 * x) + (lp_b1 * this.lp_x1[ch]) + (lp_b2 * this.lp_x2[ch]) - (lp_a1 * this.lp_y1[ch]) - (lp_a2 * this.lp_y2[ch]);
                        this.lp_x2[ch] = this.lp_x1[ch]; this.lp_x1[ch] = x;
                        this.lp_y2[ch] = this.lp_y1[ch]; this.lp_y1[ch] = y;
                        filtered = y;

                        // 4. Artefatti Oculari
                        if ((ch === 1 || ch === 2) && Math.abs(filtered) > 150) {
                            filtered = 0.0; 
                        }

                        // 5. Scrittura Buffer Circolare
                        const idx = (this.writeIndex[ch] + 1) % this.BUFFER_SIZE;
                        this.writeIndex[ch] = idx;
                        this.realBuffers[ch][idx] = filtered; 
                        this.sampleCounts[ch] = Math.min(this.sampleCounts[ch] + 1, this.BUFFER_SIZE);
                    }
                }
            }

            if(this.sampleCounts[0] >= this.BUFFER_SIZE){
                this._runFFTAndEmit();
                for(let i = 0; i < this.channelCount; i++) this.sampleCounts[i] = 0;
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
                real[i] = src[(head + i) % N];
                imag[i] = 0.0;
            }
            this._computeRadix2FFT(real, imag);

            const mags = this.fftMags[ch];
            for(let i=0; i<N/2; i++) mags[i] = Math.sqrt(real[i]*real[i] + imag[i]*imag[i]) / N;
        }

        let alphaSum = 0, betaSum = 0;
        for(let ch=0; ch<this.channelCount; ch++){
            const mags = this.fftMags[ch];
            for(let b=8; b<=12; b++) alphaSum += mags[b] || 0;
            for(let b=13; b<=30; b++) betaSum += mags[b] || 0;
        }
        const alphaMean = alphaSum / (5 * this.channelCount);
        const betaMean = betaSum / (18 * this.channelCount);

        const focusRatio = betaMean / (alphaMean + 0.001);
        let newFocus = focusRatio * this.FOCUS_SCALE;
        newFocus = Math.max(0, Math.min(255, newFocus));
        this.focusEMA = this.focusEMA * 0.8 + newFocus * 0.2;

        // --- SCRITTURA NEI BUFFER STATICI DEL POOL (ZERO ALLOCATIONS) ---
        for (let ch = 0; ch < this.channelCount; ch++) {
            this.outFFTMags[ch].set(this.fftMags[ch]);
            this.outLastSamples[ch] = this.realBuffers[ch][this.writeIndex[ch]] || 0;
        }

        this.brainDataPayload.timestamp = Date.now();
        this.brainDataPayload.focus = Math.round(this.focusEMA);
        this.brainDataPayload.alpha = Math.min(100, (alphaMean / this.MAX_UV) * 100);
        this.brainDataPayload.beta = Math.min(100, (betaMean / this.MAX_UV) * 100);

        if(typeof this.onEEGDataCallback === 'function'){
            this.onEEGDataCallback(this.brainDataPayload);
        }
    }

    _get14BitSigned(payload, bitOffset){
        const byteOffset = bitOffset >> 3;
        const bitShift = bitOffset & 7;
        const b0 = payload[byteOffset] || 0;
        const b1 = payload[byteOffset + 1] || 0;
        const b2 = payload[byteOffset + 2] || 0;
        let val = (b0 | (b1 << 8) | (b2 << 16)) >> bitShift;
        val &= 0x3FFF;
        if(val > 8191) val -= 16384; 
        return val;
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
                // --- UTILIZZO DELLA LUT PRE-CALCOLATA ---
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