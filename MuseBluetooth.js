class MuseBluetooth {
    constructor() {
        this.SERVICE_UUID = '0000fe8d-0000-1000-8000-00805f9b34fb';
        this.CONTROL_UUID = '273e0001-4c4d-454d-96be-f03bac821358';
        this.EEG_UUID = '273e0013-4c4d-454d-96be-f03bac821358';

        this.device = null;
        this.server = null;
        this.controlChar = null;
        this.eegChar = null;

        this.onEEGDataCallback = null;
        this.onDisconnectCallback = null;

        // Constants for tuning
        this.FS = 256;
        this.BUFFER_SIZE = 256;
        this.MAX_UV = 50; // Empirical max uV for alpha/beta bar percentage mapping
        this.FOCUS_SCALE = 150; // Multiplier for Beta/Alpha ratio to reach 0-255 range

        // Zero GC Buffers
        this.realBuffer = new Float32Array(this.BUFFER_SIZE);
        this.fftReal = new Float32Array(this.BUFFER_SIZE);
        this.fftImag = new Float32Array(this.BUFFER_SIZE);

        this.lastFftCounter = -1;
        this.focusEMA = 0;

        this._handleDisconnect = this._handleDisconnect.bind(this);
        this._parseEEGStream = this._parseEEGStream.bind(this);
    }

    onEEGData(callback) {
        this.onEEGDataCallback = callback;
    }

    onDisconnect(callback) {
        this.onDisconnectCallback = callback;
    }

    async connect() {
        try {
            console.log('Requesting Muse Bluetooth Device...');
            this.device = await navigator.bluetooth.requestDevice({
                filters: [{ services: [this.SERVICE_UUID] }]
            });

            this.device.addEventListener('gattserverdisconnected', this._handleDisconnect);

            console.log('Connecting to GATT Server...');
            this.server = await this.device.gatt.connect();

            console.log('Getting Service...');
            const service = await this.server.getPrimaryService(this.SERVICE_UUID);

            console.log('Getting Characteristics...');
            this.controlChar = await service.getCharacteristic(this.CONTROL_UUID);
            this.eegChar = await service.getCharacteristic(this.EEG_UUID);

            // Subscribe to Control Characteristic (CRITICAL for Athena firmware)
            console.log('Subscribing to Control notifications...');
            await this.controlChar.startNotifications();
            this.controlChar.addEventListener('characteristicvaluechanged', (e) => {
                // We just listen to keep the connection alive, logging is optional
                // console.log('Control char response received');
            });

            // Subscribe to EEG
            console.log('Subscribing to EEG notifications...');
            await this.eegChar.startNotifications();
            this.eegChar.addEventListener('characteristicvaluechanged', this._parseEEGStream);

            // Handshake sequence (Specific order derived from muse(1).py)
            console.log('Starting Handshake...');
            
            await this._sendCommand('v1');
            await this._sleep(200);
            
            await this._sendCommand('p1041');
            await this._sleep(200);
            
            await this._sendCommand('s');
            
            console.log('Muse connected and streaming!');
        } catch (error) {
            console.error('Connection failed', error);
            this._handleDisconnect();
        }
    }

    async disconnect() {
        if (this.device && this.device.gatt.connected) {
            this.device.gatt.disconnect();
        }
    }

    _handleDisconnect() {
        console.log('Muse disconnected.');
        if (this.onDisconnectCallback) {
            this.onDisconnectCallback();
        }
    }

    async _sendCommand(cmd) {
        // Formato stringa pura con '\n' finale
        const encoder = new TextEncoder();
        const strBytes = encoder.encode(cmd + '\n');
        
        // OpenMuse specifica che il payload di controllo necessita del byte di lunghezza iniziale
        const payload = new Uint8Array(strBytes.length + 1);
        payload[0] = strBytes.length; // Prefix length
        payload.set(strBytes, 1);
        
        try {
            // Usa always writeValueWithoutResponse per i command di controllo
            await this.controlChar.writeValueWithoutResponse(payload);
        } catch (e) {
            console.warn("writeValueWithoutResponse failed, trying writeValue", e);
            await this.controlChar.writeValue(payload);
        }
    }

    _sleep(ms) {
        return new Promise(resolve => setTimeout(resolve, ms));
    }

    // Unpacks 14-bit signed integer from continuous bit stream
    _get14BitSigned(payload, bitOffset) {
        const byteOffset = bitOffset >> 3;
        const bitShift = bitOffset & 7;
        const b0 = payload[byteOffset];
        const b1 = byteOffset + 1 < payload.length ? payload[byteOffset + 1] : 0;
        const b2 = byteOffset + 2 < payload.length ? payload[byteOffset + 2] : 0;
        
        let val = (b0 << 16) | (b1 << 8) | b2;
        val = (val >> (10 - bitShift)) & 0x3FFF;
        
        // Two's complement for 14-bit
        if (val > 8191) val -= 16384;
        return val;
    }

    _parseEEGStream(event) {
        const dataView = event.target.value;
        // TAG 0x11 packet: 14 bytes header + 28 bytes payload
        if (dataView.byteLength < 42) return;

        // Jitter Buffer Logic: Extract hardware timestamp (packet counter)
        const packetCounter = dataView.getUint16(0, false);
        const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + 14, 28);

        // Payload contains 4 temporal samples
        for (let s = 0; s < 4; s++) {
            const bitOffset = s * 56;
            
            // Isolate frontal channels: AF7 (idx 1), AF8 (idx 2)
            const af7 = this._get14BitSigned(payload, bitOffset + 14);
            const af8 = this._get14BitSigned(payload, bitOffset + 28);
            
            const avg = (af7 + af8) / 2;
            const uv = avg * (1450.0 / 16383.0);

            // Directly insert into Circular Buffer avoiding Buffer Bloat
            const sampleIdx = (packetCounter * 4 + s) % 256;
            this.realBuffer[sampleIdx] = uv;
        }

        if (this.lastFftCounter === -1) {
            this.lastFftCounter = packetCounter;
        } else {
            let diff = packetCounter - this.lastFftCounter;
            if (diff < 0) diff += 65536;
            
            if (diff >= 8) { // 8 packets = 32 samples downsampling step
                this._runFFT(packetCounter);
                this.lastFftCounter = packetCounter;
            }
        }
    }

    _runFFT(packetCounter) {
        // Chronological reconstruction from circular buffer
        const head = (packetCounter * 4 + 4) % 256; // Oldest sample index

        for (let i = 0; i < 256; i++) {
            this.fftReal[i] = this.realBuffer[(head + i) % 256];
            this.fftImag[i] = 0;
        }

        // Execute In-Place Radix-2 FFT (Zero GC)
        this._computeRadix2FFT(this.fftReal, this.fftImag);

        let alphaSum = 0;
        for (let i = 8; i <= 12; i++) {
            alphaSum += Math.sqrt(this.fftReal[i]**2 + this.fftImag[i]**2);
        }
        const alphaMean = alphaSum / 5;

        let betaSum = 0;
        for (let i = 13; i <= 30; i++) {
            betaSum += Math.sqrt(this.fftReal[i]**2 + this.fftImag[i]**2);
        }
        const betaMean = betaSum / 18;

        // Convert to absolute amplitude
        const alphaAmp = alphaMean / 256;
        const betaAmp = betaMean / 256;

        const focusRatio = betaAmp / (alphaAmp + 0.001);
        
        let newFocus = focusRatio * this.FOCUS_SCALE;
        newFocus = Math.max(0, Math.min(255, newFocus));

        // EMA Smoothing
        this.focusEMA = this.focusEMA * 0.8 + newFocus * 0.2;

        const alphaPct = Math.min(100, (alphaAmp / this.MAX_UV) * 100);
        const betaPct = Math.min(100, (betaAmp / this.MAX_UV) * 100);

        if (this.onEEGDataCallback) {
            this.onEEGDataCallback({
                focus: Math.round(this.focusEMA),
                alpha: alphaPct,
                beta: betaPct
            });
        }
    }

    _computeRadix2FFT(real, imag) {
        const n = real.length;
        
        let j = 0;
        for (let i = 0; i < n - 1; i++) {
            if (i < j) {
                let tempReal = real[i];
                let tempImag = imag[i];
                real[i] = real[j];
                imag[i] = imag[j];
                real[j] = tempReal;
                imag[j] = tempImag;
            }
            let k = n >> 1;
            while (k <= j) {
                j -= k;
                k >>= 1;
            }
            j += k;
        }
        
        const PI2 = -2 * Math.PI;
        for (let size = 2; size <= n; size <<= 1) {
            let halfSize = size >> 1;
            let angleStep = PI2 / size;
            
            for (let j = 0; j < halfSize; j++) {
                let wReal = Math.cos(j * angleStep);
                let wImag = Math.sin(j * angleStep);
                
                for (let i = j; i < n; i += size) {
                    let l = i + halfSize;
                    
                    let tReal = wReal * real[l] - wImag * imag[l];
                    let tImag = wReal * imag[l] + wImag * real[l];
                    
                    real[l] = real[i] - tReal;
                    imag[l] = imag[i] - tImag;
                    
                    real[i] += tReal;
                    imag[i] += tImag;
                }
            }
        }
    }
}
