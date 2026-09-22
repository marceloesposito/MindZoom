# Compilare Mind Zoom su Windows

Scritto l'8 settembre 2026, insieme al porto. **Niente di quanto segue è stato
eseguito su una macchina Windows**: il codice è stato scritto su un Mac, dove
MSVC non c'è. Leggilo come istruzioni, non come resoconto.

Il codice però **è stato compilato**, in croce dal Mac con mingw-w64 — tutto
tranne il BLE. Vedi "Controllare senza un PC Windows" in fondo: è la cosa da
rifare dopo ogni modifica finché la macchina vera non arriva.

## In breve

1. Copia la cartella del progetto sul PC Windows (tutta: serve anche `images/`,
   che sta fuori da `native/`).
2. Doppio clic su `native\build.bat`.
3. Se manca il compilatore, lo script dice cosa installare e da dove.

Non serve il Developer Command Prompt, non serve sapere dove sta Visual Studio,
non serve modificare nessun percorso. Lo script interroga `vswhere`, che
Microsoft mette in un posto fisso insieme a qualunque Visual Studio dal 2017.

## Cosa serve installato

**Visual Studio 2019 o più recente**, anche la versione gratuita, **con il
carico di lavoro "Sviluppo di applicazioni desktop con C++"**. Quella spunta è
l'unica cosa che conta: senza, viene installato l'IDE ma non il compilatore, ed
è l'errore più comune.

Download: <https://visualstudio.microsoft.com/it/downloads/> — in fondo alla
pagina, "Strumenti per Visual Studio" → "Build Tools per Visual Studio" se
vuoi solo compilare senza l'IDE (più leggero, ~2 GB invece di ~8).

CMake e Ninja arrivano con quel carico di lavoro. Se `build.bat` non li trova
te lo dice.

Il Windows SDK serve per Direct2D, DirectWrite, WIC e C++/WinRT (il Bluetooth):
è incluso nel carico di lavoro C++, non va cercato a parte.

## Cosa producono i due script

| | |
|---|---|
| `build.bat` | Compila in `build\`, lancia i test numerici e il selftest grafico. È quello da usare mentre si lavora. |
| `package.bat` | Compila in Release, converte le foto in JPEG, produce `dist\MindZoom\` e `dist\MindZoom.zip` da portare in mostra. |

Entrambi si fermano al primo errore e dicono a che punto.

## Il selftest

`MindZoom.exe --selftest` disegna un fotogramma che attraversa **ogni** percorso
di disegno su una finestra mai mostrata, ed esce con 0 se ci è riuscito.

Su questo porto è la verifica che conta più di tutte. Il backend Direct2D è
stato riscritto da zero contro quello macOS, e il selftest tocca tutte le
primitive del renderer: se una manca o sbaglia, si vede lì invece che davanti
al pubblico. Lo eseguono già `build.bat` e `package.bat`, ma vale la pena
lanciarlo a mano dopo ogni modifica alla grafica.

## Cosa aspettarsi al primo tentativo

La stima iniziale era **mezza giornata di errori del compilatore**. Dopo il
controllo incrociato con mingw-w64 (sotto) è più ragionevole **un'ora o due**:
tutto il ramo Windows tranne il BLE compila e linka già.

I due errori che il controllo ha trovato erano reali e avrebbero fermato la
prima compilazione — `<objbase.h>` mancante e `M_PI` non definito — e sono già
corretti. Quello che resta da scoprire è la differenza fra gli header di
mingw-w64 e il vero Windows SDK, che c'è ma è piccola.

I punti dove è più probabile che si rompa, in ordine:

1. **`ble/muse.cpp`.** È l'unico file che il controllo incrociato **non tocca**:
   usa C++/WinRT, che è di MSVC. Non è codice nuovo — non lo modifico dal 16
   agosto, e prima compilava — ma è anche l'unico su cui non ho alcuna
   evidenza fresca.

2. **`renderer_win32.cpp`, caricamento dei font.** Usa `IDWriteFactory5` e
   `IDWriteFontSetBuilder1` (`dwrite_3.h`). Compila con gli header di mingw; se
   il Windows SDK installato è vecchio quelle interfacce potrebbero non
   esserci. In quel caso la via d'uscita rapida è far tornare `FontFile`
   sempre vuoto: i font impacchettati non si caricano e il testo esce in
   quello di sistema — brutto ma funzionante, e sblocca tutto il resto.

3. **Differenze fra compilatori.** MSVC è più severo di GCC su certe
   conversioni, e con `/W4` dirà cose che mingw tace. Sono avvisi, non errori:
   il progetto non compila con `/WX`.

## Cosa va guardato a schermo, non solo compilato

Tre cose che il compilatore non può dirti, in ordine di quanto è probabile che
siano sbagliate:

1. **Il font dei titoli.** Su macOS l'accento è Iowan Old Style, che lì è di
   sistema e su Windows non esiste. Il renderer sceglie Constantia, poi
   Georgia (`pickAccentFamily`). È una scelta fatta su base tipografica, **non
   su un provino affiancato** come quello che aveva portato a Iowan. È l'unico
   punto in cui le due piattaforme mostrano per forza qualcosa di diverso.

2. **Le ombre.** `fillRectShadow` traduce il raggio di sfocatura di Core
   Graphics in una sigma con il rapporto usuale `sigma ≈ raggio/2`. È una
   corrispondenza numerica, non verificata affiancando i due schermi. Se
   l'ombra esce più dura o più molle di quella del Mac, il fattore è lì.

3. **Il doppio schermo.** Codice scritto, mai eseguito su due monitor veri — su
   Windows come su macOS. `--proiezione-finestra` prova quasi tutto tranne
   proprio la parte che manca: enumerazione, scelta e posizionamento su un
   secondo pannello.

## Se le foto non si vedono

Gli asset in `images/` sono WebP, e il codec WebP di WIC c'è d'ufficio su
Windows 11 ma non su 10. Due strade:

- `package.bat` li converte da solo in JPEG (`mz_convert`), ed è il motivo per
  cui esiste. Il pacchetto in `dist\` non ha il problema.
- In sviluppo, se le foto non compaiono: o installi "Estensioni immagini WebP"
  dal Microsoft Store, o lanci `mz_convert` a mano.

Il renderer prova le estensioni in ordine `.jpg`, `.webp`, `.png`: basta
affiancare i JPEG, non serve togliere i WebP.

## Dove finiscono i file

| | |
|---|---|
| Legge | accanto all'eseguibile: `assets\`, `fonts\` |
| Scrive | `%LOCALAPPDATA%\MindZoom\` — `registrazioni\`, `debug\`, `schermo.txt` |

La separazione non è formale: l'eseguibile può stare in Programmi, dove un
utente non amministratore non scrive. Su macOS lo stesso codice usa
`~/Library/Application Support/MindZoom` per una ragione diversa (scrivere nel
bundle ne rompe la firma), ma la divisione è la stessa.

Se il programma si chiude male, l'ultima riga del log in `debug\` porta il
codice `MZ-X01` e gli indirizzi dello stack. Si risolvono col `.pdb` che sta
accanto all'eseguibile — tienilo, senza quello gli indirizzi non dicono niente.

## Opzioni da riga di comando

```
--riproduci FILE.mzr    rigioca una sessione registrata, senza fascia
--senza-log             non registrare questa sessione
--calibrazione          calibrazione a due fasi invece della banda adattiva
--schermi               elenca i monitor rilevati ed esce
--schermo-singolo       non usare il secondo schermo
--proiezione-finestra   proiezione simulata in una finestra
--pannello              pannello operatore all'avvio (base)
--pannello-esperto      pannello operatore, tutte le letture
--selftest              disegna un fotogramma e riporta l'esito, senza mostrare nulla
```

Sono le stesse dello shell macOS, con gli stessi nomi.

## Struttura, per orientarsi

```
src/
  core, dsp/, control/     portabili, nessuna dipendenza dal sistema
  app/experience.cpp       la logica: stato, DSP, disegno. Condivisa.
  app/shell_win32.cpp      finestre, tasti, ciclo di messaggi.   Windows
  app/shell_macos.mm       lo stesso, in Cocoa.                  macOS
  render/renderer_win32.cpp  Direct2D + DirectWrite + WIC        Windows
  render/renderer_macos.mm   Core Graphics + Core Text           macOS
  ble/muse.cpp             C++/WinRT                             Windows
  ble/muse_macos.mm        CoreBluetooth                         macOS
```

`app/main.cpp` **non viene più compilato**: è la vecchia applicazione Windows,
con logica e shell nello stesso file. Resta come riferimento durante il porto,
con un avviso in testa. Se stai per modificarlo, il file giusto è quasi
sicuramente `experience.cpp` o `shell_win32.cpp`.

## Controllare senza un PC Windows

Finché la macchina vera non arriva, si può compilare in croce da un Mac con
**mingw-w64**. Non sostituisce MSVC, ma prende la stragrande maggioranza degli
errori — quelli che altrimenti si scoprono uno alla volta la prima mattina
davanti al PC.

```sh
brew install mingw-w64
cd native
./tools/verifica-windows.sh
```

Lo script compila ogni sorgente del ramo Windows e prova il link. Cosa copre:

| | |
|---|---|
| Compila | `renderer_win32`, `shell_win32`, `crash_win32`, `platform_win32`, `experience`, `telemetry`, `displays`, `stft`, `gating`, `calibration`, `zoom` |
| Linka | tutto quanto sopra, con un BLE finto: verifica che non manchi nessuna definizione |
| **Non** copre | `ble/muse.cpp` — C++/WinRT è di MSVC e mingw non ce l'ha |

### Le due finzioni, e perché non falsano il risultato

Lo script si appoggia a due sostituti, che stanno in `tools/verifica-windows/`
e **non entrano mai nella build vera**:

- **`winrt/base.h`**, una sessantina di righe che riproducono la sola
  `winrt::com_ptr` con le firme identiche a quelle vere (`put`, `get`,
  `try_as`, conversione a `bool`). Serve perché C++/WinRT non c'è in mingw. Se
  il codice usa `com_ptr` in un modo che non compila, non compila anche qui.
- **un `MuseClient` finto**, che definisce i simboli e basta. Il selftest non
  usa la fascia, quindi non manca niente al disegno.

Il limite vero, da tenere presente: gli header di mingw-w64 non sono quelli
del Windows SDK. Sono completi (`dwrite_3.h` incluso), ma qualche differenza
c'è. Un errore che esce qui è quasi certamente vero; **l'assenza di errori non
è una garanzia**, è un forte indizio.

### E con Wine?

Tentato, non riuscito. Il link produce un `MindZoom.exe` vero, e in teoria
`MindZoom.exe --selftest` sotto Wine direbbe se la catena Direct2D disegna
davvero. Ma tutti i pacchetti Wine di Homebrew sono **disabilitati dal 1
settembre 2026** perché non passano il controllo Gatekeeper di macOS; resterebbe
solo scaricare il `.pkg` da winehq.org aggirando Gatekeeper a mano.

Vale la pena saperlo prima di provarci: l'implementazione di Direct2D in Wine è
parziale, e quella di DirectWrite lo è di più — `IDWriteFactory5` e il font set
builder molto probabilmente non ci sono. Quindi **un successo direbbe qualcosa,
un fallimento no**: non si saprebbe distinguere un difetto nostro da un pezzo
di Wine che manca. È un controllo asimmetrico, e per questo non è la priorità.
