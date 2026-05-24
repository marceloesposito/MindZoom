# Mind Zoom - Integrazione Sensore

Questo documento serve come promemoria per i passaggi necessari quando si passerà dalla simulazione tramite mouse all'integrazione di un sensore reale (es. EEG per la concentrazione).

## Architettura dell'Input

L'applicazione è stata strutturata in modo da avere un'interfaccia standardizzata e disaccoppiata dal sistema di rendering visivo. 

Qualsiasi input, che provenga dalla rotellina del mouse o da un hardware esterno, passa attraverso un'unica funzione:

```javascript
updateFocusFromSensor(rawValue)
// In alternativa, esposta globalmente come: window.setSensorValue(rawValue)
```

Il parametro `rawValue` accetta un **qualsiasi valore intero o decimale compreso tra 0 e 255**. La funzione si occupa in automatico di limitare i valori fuori soglia e di normalizzarli per l'animazione.

## Passaggi per l'Implementazione Reale

Quando avrai il sensore reale pronto e in comunicazione con l'applicazione (es. via WebSocket o API locale), segui questi due step:

### 1. Disabilitare la Simulazione

Apri il file `app.js` e cerca la funzione `handleContinueSimulation()`. 
Commenta o rimuovi la riga che attiva l'ascolto della rotellina del mouse:

```javascript
function handleContinueSimulation() {
    console.log('🎮 Activating simulation mode');
    AppState.isSimulationMode = true;
    closeAlertModal();
    switchToImmersion();

    // Commenta questa riga per disabilitare il mouse!
    // activateSimulationMode(); 
}
```
*(Nota: il modulo di simulazione si trova sotto l'intestazione `// MODULO SIMULAZIONE (Scollegabile)`. Puoi lasciarlo nel codice senza problemi, se non viene attivato non consuma risorse).*

### 2. Inviare i dati dal sensore

Nel punto del tuo codice in cui ricevi i dati grezzi dal sensore (ad esempio nel listener del WebSocket, nella funzione `initializeWebSocket`), chiama direttamente la funzione di interfaccia passandogli il valore di concentrazione mappato da 0 a 255.

```javascript
// Esempio fittizio di ricezione dati da sensore
function onSensorDataReceived(data) {
    // 1. Leggi il livello di concentrazione
    let concentrationLevel = data.focusValue; 
    
    // 2. (Opzionale) Mappa il valore se il sensore non restituisce 0-255
    // let mappedValue = mappaValori(concentrationLevel, min, max, 0, 255);
    
    // 3. Invia il valore all'interfaccia dell'app
    updateFocusFromSensor(concentrationLevel);
}
```

### 3. Modificare l'avvio (Opzionale)
Attualmente la transizione all'Immersione senza dispositivi avviene cliccando "Prosegui con simulazione" nel modale di errore. Quando il sensore sarà collegato, il WebSocket setterà a `true` le variabili di connessione e il tasto "Begin the immersion" effettuerà la transizione automaticamente. Assicurati che l'invio dei dati (Step 2) sia all'interno del flusso dati valido del tuo ponte hardware.
