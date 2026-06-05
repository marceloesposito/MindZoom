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
        // Muse S exposes only 3 EEG characteristics; map 3->4 logical channels.
        this.EEG_UUIDS = [
            '273e0013-4c4d-454d-96be-f03bac821358', // TP9
            '273e0014-4c4d-454d-96be-f03bac821358', // AF7
            '273e0015-4c4d-454d-96be-f03bac821358'  // AF8 (+ TP10 multiplexed on Muse S)
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
        this.realBuffers = Array.from({length: this.channelCount}, ()=> new Float32Array(this.BUFFER_SIZE));
        this.writeIndex = new Uint32Array(this.channelCount); // per-channel write pointer
        this.sampleCounts = new Uint32Array(this.channelCount);

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
                    console.log('[MuseBluetooth] Control notify', dv.byteLength, 'bytes:', preview, dv.byteLength>32? '...':'');
                }catch(err){ console.warn('[MuseBluetooth] Control notify parse failed', err); }
            });
            console.log('Control notifications started', this.CONTROL_UUID);

            // Subscribe to configured EEG characteristics (tolerant: Muse S exposes 3 UUIDs)
            for(let i=0;i<this.EEG_UUIDS.length;i++){
                const uuid = this.EEG_UUIDS[i];
                try{
                    const char = await service.getCharacteristic(uuid);
                    await char.startNotifications();
                    char.addEventListener('characteristicvaluechanged', (e)=> this.handleIncomingEEGPacket(uuid, e.target.value));
                    this.eegChars[uuid] = char;
                    console.log('[MuseBluetooth] EEG notifications started', uuid);
                }catch(err){
                    // Non-fatal: device may not expose all UUIDs; continue
                    console.warn('[MuseBluetooth] Failed to subscribe EEG char (ignored):', uuid, err && err.name);
                }
            }

            // Build mapping from characteristic UUID -> logical channel indices
            // For Muse S (3 characteristics) we assume distribution: 0013->TP9, 0014->AF7, 0015->AF8+TP10 (interleaved)
            this.charToChannelMap = {};
            const availableUUIDs = Object.keys(this.eegChars);
            if(availableUUIDs.length === 0) console.warn('[MuseBluetooth] No EEG characteristics subscribed');
            for(let i=0;i<availableUUIDs.length;i++){
                const u = availableUUIDs[i].toLowerCase();
                if(i === 0) this.charToChannelMap[u] = { channels: [0], interleaved: false };
                else if(i === 1) this.charToChannelMap[u] = { channels: [1], interleaved: false };
                else if(i === 2) this.charToChannelMap[u] = { channels: [2,3], interleaved: true };
                else this.charToChannelMap[u] = { channels: [i], interleaved: false };
            }

            // Handshake sequence (timed)
            console.log('[MuseBluetooth] Starting Handshake...');
            await this.sendControlCommand('v1');
            await this._sleep(500);
            await this.sendControlCommand('p1041');
            await this._sleep(500);
            await this.sendControlCommand('s');
            await this._sleep(200);
            console.log('[MuseBluetooth] Stream activated successfully.');

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
    // value is expected to be a DataView (e.target.value) but we accept ArrayBuffer-like too.
    handleIncomingEEGPacket(uuid, value){
        try{
            const u = (''+uuid).toLowerCase();
            const chIndex = this.EEG_UUIDS.findIndex(x => x.toLowerCase() === u);
            if(chIndex < 0) return console.warn('[MuseBluetooth] Received EEG packet from unknown UUID', uuid);

            // Normalize DataView
            let dataView = value;
            if(!(dataView && typeof dataView.getUint8 === 'function')){
                // Possibly ArrayBuffer or Uint8Array
                const arr = new Uint8Array(value);
                dataView = new DataView(arr.buffer);
            }

            const len = dataView.byteLength || 0;
            console.debug('[MuseBluetooth] EEG notify', uuid, 'len=', len);

            // Emit raw bytes for debugging
            try{
                const raw = new Uint8Array(dataView.buffer, dataView.byteOffset, len);
                const preview = Array.from(raw.slice(0,32)).map(b=>b.toString(16).padStart(2,'0')).join(' ');
                console.log('[MuseBluetooth] RAW ('+uuid+')', len, 'bytes:', preview, len>32? '...':'');
                if(typeof this.onRawDataCallback === 'function') this.onRawDataCallback(new Uint8Array(raw));
                this._lastRawAt = Date.now();
            }catch(e){ console.warn('[MuseBluetooth] failed to emit raw data callback', e); }

            // Minimal validation: expect at least 3 bytes (2-byte counter + 1 payload)
            if(len < 3) return;

            // Extract packet counter (big-endian)
            const packetCounter = dataView.getUint16(0, false);

            // Payload follows the first 2 bytes
            const payloadLen = len - 2;
            const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + 2, payloadLen);

            // Count how many 14-bit signed samples we can extract
            const totalBits = payload.length * 8;
            const samplesAvailable = Math.floor(totalBits / 14);
            if(samplesAvailable <= 0) return;

            // Extract sequential 14-bit signed values
            for(let s=0; s<samplesAvailable; s++){
                const bitOffset = s * 14;
                const rawVal = this._get14BitSigned(payload, bitOffset);
                // Convert to microvolts using empirical scaling used by OpenMuse/Athena
                const uv = rawVal * (1450.0 / 16383.0);

                // Write into per-channel circular buffer
                const idx = (this.writeIndex[chIndex] + 1) % this.BUFFER_SIZE;
                this.writeIndex[chIndex] = idx;
                this.realBuffers[chIndex][idx] = uv;
                this.sampleCounts[chIndex] = Math.min(this.sampleCounts[chIndex] + 1, this.BUFFER_SIZE);
            }

            // Decide whether to run FFT+metrics: do it when this channel has accumulated BUFFER_SIZE samples
            if(this.sampleCounts[chIndex] >= this.BUFFER_SIZE){
                this._runFFTAndEmit();
            }

        }catch(err){ console.error('[MuseBluetooth] handleIncomingEEGPacket failed', err); }
    }

    // Compute per-channel FFTs using the circular buffers and emit aggregated metrics
    _runFFTAndEmit(){
        const N = this.BUFFER_SIZE;
        // For each channel, reconstruct chronological buffer and run FFT
        for(let ch=0; ch<this.EEG_UUIDS.length; ch++){
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

        // Aggregate Alpha (8-12) and Beta (13-30) across channels
        let alphaSum = 0, betaSum = 0;
        for(let ch=0; ch<this.EEG_UUIDS.length; ch++){
            const mags = this.fftMags[ch];
            for(let b=8; b<=12; b++) alphaSum += mags[b] || 0;
            for(let b=13; b<=30; b++) betaSum += mags[b] || 0;
        }
        const alphaMean = alphaSum / (5 * this.EEG_UUIDS.length);
        const betaMean = betaSum / (18 * this.EEG_UUIDS.length);

        const alphaAmp = alphaMean;
        const betaAmp = betaMean;
        const focusRatio = betaAmp / (alphaAmp + 0.001);
        let newFocus = focusRatio * this.FOCUS_SCALE;
        newFocus = Math.max(0, Math.min(255, newFocus));
        this.focusEMA = this.focusEMA * 0.8 + newFocus * 0.2;

        const alphaPct = Math.min(100, (alphaAmp / this.MAX_UV) * 100);
        const betaPct = Math.min(100, (betaAmp / this.MAX_UV) * 100);

        // Last samples per channel (most recent)
        const lastSamples = this.EEG_UUIDS.map((_,ch)=> this.realBuffers[ch][this.writeIndex[ch]] || 0);

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
        // payload: Uint8Array
        const byteOffset = bitOffset >> 3;
        const bitShift = bitOffset & 7;
        const b0 = payload[byteOffset] || 0;
        const b1 = payload[byteOffset + 1] || 0;
        const b2 = payload[byteOffset + 2] || 0;

        // build 24-bit window and shift
        let val = (b0 << 16) | (b1 << 8) | b2;
        val = (val >> (10 - bitShift)) & 0x3FFF; // extract 14 bits
        if(val > 8191) val -= 16384; // two's complement
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
