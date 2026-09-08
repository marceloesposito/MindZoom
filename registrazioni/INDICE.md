# Registrazioni delle sessioni

Tutte le registrazioni `.mzr` prodotte durante lo sviluppo, raccolte qui in un
posto solo. Prima erano sparse dentro i bundle `.app` di `native/build*/`, nei
pacchetti sul Desktop e in `~/Library/Application Support/MindZoom/` — cioè in
cartelle ignorate da git o fuori dalla repo, quindi a rischio di sparire alla
prossima ricompilazione.

## Cosa c'è dentro

- **`fascia/`** — 27 registrazioni di EEG vero dal Muse, 60m46s in tutto.
  Contengono anche i pacchetti Bluetooth grezzi, quindi si possono ri-decodificare
  se il decodificatore cambia. Una di queste (`sessione-20260903-195333.mzr`) è
  troncata: 7 pacchetti, zero campioni: l'app la rifiuta.

- **`sintetiche/`** — 13 registrazioni dal generatore interno, 31m51s in tutto.
  Nessun pacchetto Bluetooth, ampiezza ±1,5 µV: servono ai test, non al giudizio
  sul comportamento del controllo.


I duplicati sono stati eliminati: dei 46 file trovati, 41 hanno contenuto distinto.
La colonna "anche in" segnala dove ne esisteva una copia identica.


## Come si rigioca una registrazione

```sh
open native/build-macos/MindZoom.app --args --senza-log --riproduci <percorso/del/file.mzr>
```


## Elenco

### Con la fascia

| quando | file | durata | pacchetti | picco µV | anche in |
|---|---|---|---|---|---|
| 26/08/2026 18:44 | `sessione-20260826-184431.mzr` | 3m00s | 6589 | 725 | — |
| 26/08/2026 18:52 | `sessione-20260826-185239.mzr` | 1m23s | 3053 | 725 | — |
| 26/08/2026 19:16 | `sessione-20260826-191630.mzr` | 2m07s | 4674 | 725 | — |
| 26/08/2026 19:35 | `sessione-20260826-193501.mzr` | 2m37s | 5751 | 725 | — |
| 26/08/2026 19:50 | `sessione-20260826-195014.mzr` | 1m02s | 2316 | 725 | — |
| 26/08/2026 20:27 | `sessione-20260826-202745.mzr` | 0m43s | 1629 | 725 | — |
| 26/08/2026 20:40 | `sessione-20260826-204034.mzr` | 1m18s | 2906 | 725 | — |
| 26/08/2026 21:09 | `sessione-20260826-210950.mzr` | 1m57s | 4291 | 725 | — |
| 28/08/2026 15:17 | `sessione-20260828-151715.mzr` | 2m03s | 4524 | 725 | — |
| 28/08/2026 15:40 | `sessione-20260828-154022.mzr` | 4m54s | 10661 | 725 | — |
| 28/08/2026 16:15 | `sessione-20260828-161500.mzr` | 1m56s | 4242 | 725 | — |
| 28/08/2026 16:30 | `sessione-20260828-163050.mzr` | 4m36s | 10047 | 725 | — |
| 28/08/2026 17:41 | `sessione-20260828-174123.mzr` | 7m31s | 16390 | 725 | — |
| 28/08/2026 18:38 | `sessione-20260828-183803.mzr` | 0m56s | 2114 | 304 | — |
| 01/09/2026 20:20 | `sessione-20260901-202038.mzr` | 2m00s | 4413 | 725 | ~/Documents/GitHub/MindZoom/native/dist/MindZoom-macOS/MindZoom.app/Contents/MacOS/registrazioni/sessione-20260901-202038.mzr |
| 01/09/2026 20:32 | `sessione-20260901-203221.mzr` | 2m19s | 5097 | 725 | — |
| 01/09/2026 21:07 | `sessione-20260901-210736.mzr` | 2m04s | 4551 | 725 | — |
| 03/09/2026 12:55 | `sessione-20260903-125552.mzr` | 4m34s | 9991 | 725 | — |
| 03/09/2026 14:05 | `sessione-20260903-140529.mzr` | 0m15s | 603 | 725 | — |
| 03/09/2026 14:06 | `sessione-20260903-140607.mzr` | 3m32s | 7770 | 725 | — |
| 03/09/2026 19:50 | `sessione-20260903-195054.mzr` | 2m07s | 4673 | 725 | — |
| 03/09/2026 19:53 | `sessione-20260903-195333.mzr` ⚠️ | 0m00s | 7 | 0 | — |
| 03/09/2026 20:00 | `sessione-20260903-200048.mzr` | 1m29s | 3299 | 725 | — |
| 03/09/2026 20:16 | `sessione-20260903-201626.mzr` | 0m09s | 410 | 725 | — |
| 03/09/2026 20:16 | `sessione-20260903-201646.mzr` | 0m05s | 252 | 725 | — |
| 05/09/2026 21:59 | `sessione-20260905-215907.mzr` | 1m49s | 4023 | 649 | ~/Desktop/MINDZOOM MOSTRA (prima dei due stili)/sessione-demo.mzr<br>~/Desktop/MINDZOOM MOSTRA/MindZoom.app/Contents/Resources/sessione-demo.mzr |
| 08/09/2026 16:03 | `sessione-20260908-160323.mzr` | 0m32s | 1220 | 73 | — |
| — | `registrazione-demo.mzr` | 3m34s | 7806 | 725 | ~/Documents/GitHub/MindZoom/native/build/MindZoom.app/Contents/MacOS/registrazioni/sessione-20260904-115836.mzr |

### Sintetiche

| quando | file | durata | anche in |
|---|---|---|---|
| 03/09/2026 12:21 | `sessione-20260903-122129.mzr` | 0m08s | — |
| 03/09/2026 13:07 | `sessione-20260903-130740.mzr` | 8m59s | — |
| 03/09/2026 14:09 | `sessione-20260903-140956.mzr` | 6m55s | — |
| 03/09/2026 19:39 | `sessione-20260903-193954.mzr` | 1m47s | — |
| 03/09/2026 20:11 | `sessione-20260903-201128.mzr` | 0m56s | — |
| 03/09/2026 20:15 | `sessione-20260903-201505.mzr` | 0m05s | — |
| 05/09/2026 16:56 | `sessione-20260905-165614.mzr` | 0m38s | ~/Documents/GitHub/MindZoom/native/dist/MindZoom-macOS/MindZoom.app/Contents/MacOS/registrazioni/sessione-20260905-165614.mzr |
| 05/09/2026 20:18 | `sessione-20260905-201805.mzr` | 0m20s | — |
| 05/09/2026 22:06 | `sessione-20260905-220647.mzr` | 0m35s | — |
| 07/09/2026 21:24 | `sessione-20260907-212406.mzr` | 1m13s | — |
| — | `sessione-di-prova.mzr` | 4m00s | — |
| — | `sintetica.mzr` | 4m00s | — |
| — | `sintetica2.mzr` | 2m10s | — |

## Una nota sui valori di picco

Quasi tutte le sessioni con la fascia toccano i **725 µV**, cioè il fondo scala
dell'ADC del Muse: sono artefatti da movimento, contatto o battito di ciglia, non
il segnale utile. Fanno eccezione `sessione-20260828-183803` (304 µV),
`sessione-20260905-215907` (649 µV, la registrazione scelta per la demo di sala) e
soprattutto **`sessione-20260908-160323` (73 µV)**, l'unica che non satura mai.

