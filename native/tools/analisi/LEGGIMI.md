# Strumenti d'analisi delle registrazioni

Tre programmi usa-e-getta che rigiocano un file `.mzr` attraverso la **vera** catena
DSP dell'applicazione (STFT → gating → indice di Pope → smoother → banda → legge di
controllo) e riportano cosa avrebbe fatto il controllo.

Non fanno parte dell'applicazione e non sono in `CMakeLists.txt`: si compilano a mano
quando servono. Esistono perché una domanda come *"è plausibile che non riuscissi a
fare zoom out?"* non si risponde leggendo il codice — si risponde misurando.

## Compilare

```sh
cd native
clang++ -std=c++20 -O2 -I src tools/analisi/analizza.cpp build-macos/libmz_core.a -o /tmp/analizza
```

Stessa riga per `analizza_adattiva.cpp` e `sweep.cpp`.

## Usare

```sh
/tmp/analizza build-macos/MindZoom.app/Contents/MacOS/registrazioni/sessione-*.mzr
```

Le registrazioni stanno in `MindZoom.app/Contents/MacOS/registrazioni/`, il log di
diagnostica per sessione in `.../debug/mindzoom-debug-*.log`.

## Cosa fa ciascuno

| file | domanda a cui risponde |
|---|---|
| `analizza.cpp` | con la **calibrazione a due fasi**: che banda esce, a che percentile della distribuzione cade il neutro, quanto tempo si sta sopra/sotto, quanto zoom in e out sono disponibili, quanto deriva l'indice fra il primo e l'ultimo terzo |
| `analizza_adattiva.cpp` | gli stessi indicatori con la **banda adattiva**, per metterli accanto |
| `sweep.cpp` | spazza la **lunghezza della finestra** della banda adattiva su una registrazione: serve a scegliere `config::kAdaptiveWindowS` guardando i numeri invece che a intuito |

## Cosa hanno gia' dimostrato (28/08/2026)

Su quattro sessioni con la fascia reale, la calibrazione a estremi:

- mette il neutro al 19°, 21°, 47° e 100° percentile — cioè in un punto arbitrario;
- sbaglia con un **verso sistematico**: la fase più lunga produce l'estremo più
  estremo, e Relax dura sempre più di Concentrazione, quindi il neutro viene tirato in
  basso e si finisce col 78% del tempo sopra il neutro e lo zoom out disponibile solo
  nel 17%;
- non insegue la deriva dell'indice (+36% e +64% durante una sessione).

E hanno ribaltato una scelta di progetto: la finestra della banda adattiva a 150s,
scelta a intuito, migliorava solo una sessione su tre. Lo sweep ha mostrato che serviva
45s, e ha rivelato la tensione fra "il neutro resta al centro" (finestra corta) e
"concentrarsi a lungo porta avanti davvero" (finestra lunga).

## Valori di riferimento dalla letteratura (01/09/2026)

Finora "plausibile" voleva dire solo "internamente coerente" (la banda si comporta
come dovrebbe, il neutro sta in un punto sensato). Da questa sessione in poi, quando si
analizza una registrazione va anche confrontata con valori pubblicati per persone in
generale - non solo con se stessa. Quello che c'e' in letteratura:

**Indice di Pope (quello che l'app usa davvero)**: `20 * beta/(alpha+theta)`, Pope,
Bogart & Bartolome 1995 (NASA, originariamente per misurare l'engagement dei piloti).
La formula e' confermata dalla fonte NASA (NTRS 19970003078), ma il paper NON pubblica
un "valore normale" assoluto: e' pensato per essere letto RELATIVO allo stesso
soggetto nel tempo, mai confrontato fra persone diverse su scala assoluta. Questo in
realta' CONFERMA la scelta gia' fatta in questo progetto (calibrazione/banda adattiva
personale invece di soglie fisse) invece di darle un numero contro cui misurarsi.

**Theta/Beta Ratio (TBR)**: parente stretta (stessa famiglia beta vs onde lente), ma
denominatore diverso - theta soltanto, non alpha+theta - quindi i numeri NON sono
direttamente comparabili all'indice di Pope, solo la direzione (piu' beta relativo =
piu' attivazione) lo e'. E' pero' l'unica di questa famiglia con valori normativi
pubblicati:
  - Van Son, van der Does, Band & Putman (2020), *Applied Psychophysiology and
    Biofeedback* 45(3):195-210: TBR frontale/centrale in adulti sani, campione non
    selezionato (n=56): media 1.26 (DS 0.54); un sottogruppo selezionato per TBR alto
    (n=12): 1.51-1.68. Bande: theta 4-7 Hz, beta 13-30 Hz - quasi identiche a
    `config::kThetaLo/Hi` (4-8) e `kBetaLo/Hi` (13-30) di questo progetto.
  - Il TBR alto e' storicamente associato a scarso controllo attentivo/ADHD nella
    letteratura clinica; un TBR molto piu' alto del range 1.2-1.7 durante una fase di
    Concentrazione sarebbe quindi il contrario di quel che ci si aspetta - un segnale
    da controllare, non solo una banda "diversa dalla propria media".

**Desincronizzazione alpha (ERD)**: la potenza alpha scende (rispetto al proprio
basale) durante compiti che richiedono attenzione - direzione consistente in tutta la
letteratura (Pfurtscheller & Lopes da Silva e chi li ha seguiti), ma riportata come
percentuale di calo rispetto al PROPRIO basale, non come soglia assoluta. Utile per un
controllo di plausibilita' direzionale (alpha scende durante Concentrate rispetto a
Relax?), non come numero di riferimento.

**Perche' il confronto resta comunque limitato**: le tre fonti sopra usano montaggi
riferenziali (Cz, frontali) misurati contro un riferimento comune, mentre questa app
legge la derivazione BIPOLARE AF7-AF8 (`config::kBipolar`, vedi il commento in
`dsp/stft.cpp` sul perche') apposta per cancellare il ronzio di rete. Un confronto
assoluto di potenza in microvolt^2 fra le due configurazioni non ha senso; quello che
si puo' prendere in prestito e' la FORMA (bande in Hz, direzione degli effetti,
ordine di grandezza dei rapporti fra bande), non il numero esatto.

**Come usarli in pratica**: quando si analizza una sessione registrata, oltre agli
indicatori interni (percentile del neutro, tempo sopra/sotto, deriva fra terzi - vedi
sopra), calcolare anche il TBR medio nelle fasi di Concentrazione e Relax e leggerlo
contro il range 1.2-1.7: se una fase "di relax" misurata risulta con TBR piu' basso di
quella "di concentrazione" e' il verso sbagliato, e vale la pena chiedersi se il
soggetto ha davvero fatto quello che gli era stato chiesto (o se la fase e' troppo
corta/rumorosa per fidarsene) prima di fidarsi del resto dei numeri.

Fonti: [Pope, Bogart & Bartolome 1995, NASA NTRS 19970003078](https://ntrs.nasa.gov/citations/19970003078) ·
[Van Son et al. 2020, Appl Psychophysiol Biofeedback 45(3):195-210](https://pmc.ncbi.nlm.nih.gov/articles/PMC7391399/) ·
[Raufi & Longo, arXiv:2202.12937 - alpha/theta ratios come indice di carico mentale](https://arxiv.org/abs/2202.12937)

## Una sessione sana, da confrontare (05/09/2026)

`riferimento/sessione-sana-20260905.log` e' il log di diagnostica di una sessione
reale con la fascia, andata bene dall'inizio alla fine. Serve da termine di paragone
quando in mostra qualcosa non torna: si apre il log della sessione storta accanto a
questo e si guarda dove le due divergono.

Cosa deve assomigliare a questo:

- la sequenza dei codici: `MZ-B02` (ricerca) → `MZ-B03` (connessione) → `MZ-I03`
  (pagina d'ingresso) → `MZ-I02` (adattamento, ~20 s) → `MZ-00` (tutto regolare);
- le righe `adattiva:` che passano da `pronta=0 riscaldamento=0%` a `pronta=1
  riscaldamento=100%` con una banda sensata (qui `[0.63 .. 1.42] M=0.89`);
- nelle righe `[stato]`: `fresh=1 fault=0 contact=1`, `fps` a 60, e i pacchetti che
  crescono con una resa di circa due terzi (qui 2861 validi su 4023 grezzi);
- l'ultima riga `=== fine sessione (chiusura regolare) ===`. Se manca, il programma
  non e' stato chiuso con ESC/Q: o e' caduto (allora c'e' `[MZ-X01]` col suo stack)
  o e' stato interrotto dall'esterno.

Comparse di `MZ-S07` (movimento) sparse durante l'esperienza sono normali: sono i
momenti in cui la persona si e' mossa, e passano da soli.
