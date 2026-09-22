# Biodetails — identità visiva e linee guida di design

Analisi del poster "Biodetails Press Kit" (InDesign 21.5, un foglio 70 × 100 cm,
esportato in PDF) e linee guida ricavate per applicarne l'identità a Mind Zoom.
Tutti i valori sono **misurati sul PDF** (colori dagli operatori del flusso di
contenuto, corpi dalle matrici di testo, griglia dalle guide salvate da
InDesign, geometrie dai tracciati vettoriali), non stimati a occhio. Dove un
valore è invece stimato lo dico esplicitamente.

Asset allegati in questa cartella:

| File | Cosa contiene |
|---|---|
| `poster-riferimento.png` | il poster renderizzato (1200 px di larghezza) |
| `titolo-e-macchia.svg` | titolo + macchia, tracciati originali estratti dal PDF, in coordinate di pagina |
| `macchia.svg` | la sola macchia, normalizzata a larghezza 1 |
| `../../native/src/app/biodetails_blob.hpp` | la macchia come 40 segmenti cubici normalizzati, per il disegno nativo |
| `../../native/assets/fonts/LeagueGothic-Variable.ttf` | il carattere sostitutivo del lettering (OFL) |

---

## 1. Il foglio e la griglia

**Formato.** 1984,25 × 2834,65 pt = **700 × 1000 mm**, verticale. Nessuna
abbondanza: ArtBox, TrimBox e MediaBox coincidono.

**Margini.** 22,677 pt = **8 mm** su tutti e quattro i lati. Le guide salvate
nel file lo confermano: i bordi della gabbia stanno a 22,68 pt da ogni lato.

**Griglia modulare 4 × 4.** Le guide InDesign definiscono quattro colonne e
quattro righe con gocce (gutter) di 8 mm, identiche ai margini:

| | misura | in mm |
|---|---|---|
| larghezza colonna | 467,7 pt | 165 mm |
| altezza riga | 680,3 pt | 240 mm |
| goccia (orizzontale e verticale) | 22,68 pt | 8 mm |

Bordi delle colonne (x, in pt dal bordo sinistro): 22,7 · 490,4 | 513,1 ·
980,8 | 1003,5 · 1471,2 | 1493,9 · 1961,6.
Bordi delle righe (y, dal bordo alto): 22,7 · 703,0 | 725,7 · 1405,8 | 1428,5 ·
2108,6 | 2131,3 · 2812,0.

Il **modulo** vale quindi 165 × 240 mm, cioè un rapporto 11:16. Il foglio
intero è 7:10. **Un solo valore (8 mm) governa margini e gocce**: questa è la
regola più semplice e più caratterizzante della gabbia.

**Come la gabbia viene usata.**

- Riga 1 (alto): il lettering "BIODETAILS" e la macchia magenta, che
  attraversano tutte e quattro le colonne.
- Riga 2: le date ("9–" / "11 September") allineate a sinistra sulla colonna 1,
  a cavallo con la riga 1.
- Riga 3: a sinistra, sottotitolo + curatrici (colonne 1–2); a destra, l'anno
  spezzato "20 / 26" (colonna 3) e la sede (dalla metà della colonna 3 alla 4).
- Riga 4 (basso): testo di presentazione su colonne 1–2 (giustificato), crediti
  su colonna 3 e colonna 4, ciascuno in un'unica colonna di testo.

**Tutto è allineato a sinistra.** Non esiste un solo elemento centrato: ogni
blocco parte da un bordo di colonna. Il ritmo verticale è dato dai bordi di
riga, non da distanze "a occhio" tra blocchi.

---

## 2. Palette

Cinque colori piatti, definiti in RGB nel file (spazio `rg`, nessun
profilo separazione). Nessuna sfumatura, nessuna trasparenza, nessuna ombra,
nessun contorno: **ogni elemento è una campitura piena di uno solo di questi
cinque colori**. L'istogramma del render lo conferma: oltre il 99 % dei pixel
appartiene a una di queste cinque tinte, il resto è antialiasing.

| Nome | HEX | RGB (0–255) | RGB (0–1) | Copertura del foglio | Ruolo |
|---|---|---|---|---|---|
| **Acqua** (fondo) | `#ABCFD5` | 171 · 207 · 213 | 0,671 · 0,812 · 0,835 | ~70 % | fondo unico di tutto il foglio |
| **Magenta** | `#FF0085` | 255 · 0 · 133 | 1 · 0 · 0,52 | ~7 % | la macchia organica, unico elemento non tipografico |
| **Corallo** | `#FF3852` | 255 · 56 · 82 | 1 · 0,22 · 0,32 | ~6 % | lettering del titolo, date, anno: la "voce" più forte |
| **Arancio** | `#FF6E1C` | 255 · 110 · 28 | 1 · 0,43 · 0,11 | ~3 % | lettering dove attraversa la macchia, sede, titoli di sezione |
| **Nero** | `#000000` | 0 · 0 · 0 | 0 · 0 · 0 | ~2 % | sottotitolo, curatrici, testo corrente, elenchi |

Osservazioni:

- I tre caldi hanno **rosso = 255 e blu basso**: sono la stessa famiglia
  "fluo" a tre temperature (magenta → corallo → arancio), su un fondo freddo
  e desaturato. Il contrasto identitario è caldo-saturo contro freddo-spento.
- **Non ci sono grigi né bianchi.** La gerarchia del testo si fa con corpo,
  peso e colore (arancio per i titoletti, nero per il resto), mai con toni
  attenuati. Il bianco non appare nemmeno come carta: il fondo è colorato da
  bordo a bordo.
- Il nero è usato **solo per il testo**, mai per forme o filetti.

**Contrasti WCAG (rapporto di luminanza)**, utili per la traduzione a schermo:

| Coppia | Rapporto | Nota |
|---|---|---|
| nero su acqua | 12,6 | testo corrente: ottimo |
| arancio su nero | 7,5 | — |
| corallo su nero | 5,9 | — |
| magenta su nero | 5,6 | — |
| magenta su acqua | 2,3 | solo forme grandi |
| corallo su acqua | 2,1 | solo corpi grandi (titolo, date) |
| arancio su acqua | 1,7 | solo corpi grandi o etichette brevi |
| arancio su magenta | 1,3 | solo il lettering gigante |

Il poster usa i caldi su acqua **solo a corpi molto grandi** (da 21 pt su
70 cm in su, cioè titoletti brevissimi) e il nero per tutto ciò che va letto
davvero. È una regola da rispettare anche a schermo: colore caldo = titolo,
etichetta, numero; nero = lettura.

**Combinazioni ammesse** (le sole che compaiono nel foglio):

- corallo / arancio / magenta / nero **su acqua**;
- arancio **su magenta** (solo il lettering che attraversa la macchia);
- nulla su corallo, nulla su arancio, mai due caldi affiancati come campiture.

---

## 3. Tipografia

### 3.1 Famiglie

Due sole voci tipografiche, tre facce:

| Uso | Carattere | Faccia nel PDF |
|---|---|---|
| tutto il testo, dalle date al colophon | **Helvetica Neue** | `HelveticaNeue` (55 Roman), `HelveticaNeue-Medium` (65), `HelveticaNeue-Italic` (una sola parola: "One Health") |
| il lettering "BIODETAILS" | tracciati vettoriali (nessun font incorporato) | vedi § 4 |

Helvetica Neue è usata con **due soli pesi**: Roman per tutto, Medium solo per
i titoletti di sezione ("Promoted by", "Curated by", "Visual Design"…). Niente
Bold, niente Light. Il corsivo compare una volta sola, per un termine
straniero nel corpo.

### 3.2 Scala dei corpi

Corpi misurati (in pt sul foglio 70 × 100) con interlinea e colore:

| Livello | Corpo | Interlinea | Rapporto | Peso | Colore | Dove |
|---|---|---|---|---|---|---|
| Display | **280** | 250 | 0,89 (negativa) | Roman | corallo | "9–", "11 September", "20", "26" |
| Titolo | **86** | 100 | 1,16 | Roman | nero | sottotitolo "Convergences between…" |
| Titolo | **86** | 77 | 0,90 (negativa) | Roman | arancio | sede "Casa Schuster, Centro Ambrosiano…" |
| Nome | **43** | — | — | Roman | nero | curatrici |
| Corpo | **23** | 25 | 1,09 | Roman | nero | testo di presentazione |
| Titoletto | **21** | — | — | **Medium** | arancio (nero per "Curated by") | intestazioni delle sezioni |
| Colophon | **16** | 19,2 | 1,20 | Roman | nero | elenchi di nomi |

La scala è quasi geometrica con ragione ≈ 1,3–2: 16 → 21 → 23 → 43 → 86 →
280. In rapporto all'altezza del foglio: display = 9,9 %, titolo = 3,0 %,
corpo = 0,81 %, colophon = 0,56 %.

**Interlinee negative sui grandi.** Le cifre e le righe di sede sono composte
con interlinea inferiore al corpo (0,89–0,90): le righe si toccano quasi,
l'occhio legge un blocco solido. Sui corpi di lettura l'interlinea è invece
stretta ma positiva (1,09 nel corpo, 1,20 negli elenchi).

**Spaziatura.** Sui corpi grandi il tracking è negativo (nel PDF compaiono
`Tc` di −0,01, −0,03 e −0,05 em): le lettere delle date e del sottotitolo sono
più strette del normale. Sui corpi di lettura la spaziatura è quella di
default.

### 3.3 Caso, allineamento, dettagli

- **Maiuscolo/minuscolo normale** ovunque tranne il lettering del titolo, che è
  tutto maiuscolo. Nessuna riga in maiuscoletto o versaletto.
- **Tutto bandiera sinistra**, salvo il testo di presentazione, che è
  **giustificato** a blocco su due colonne di gabbia (~958 pt di giustezza).
- Le date usano il **trattino lungo (en dash) senza spazi**: "9–" a fine riga,
  poi "11 September" a capo. L'anno è **spezzato su due righe** ("20" / "26"):
  la cifra diventa un blocco grafico.
- Le righe della sede vanno **a capo per unità di senso** (una riga per nome:
  "Casa" / "Schuster," / "Centro" / "Ambrosiano," / "Piero Panighi" / "Room,
  Milan"), non per giustezza.
- I titoletti di sezione stanno **da soli su una riga** in arancio Medium, e
  l'elenco nero segue subito sotto senza spazio aggiuntivo; fra una sezione e
  la successiva c'è una riga vuota abbondante (≈ 2 interlinee).

---

## 4. Il lettering "BIODETAILS"

È l'elemento identitario più forte, e non è un font: nel PDF sono **dieci
tracciati vettoriali** (le due "I" sono rettangoli semplici), quindi un
lettering o un font convertito in tracciati.

### 4.1 Geometria misurata

| Misura | Valore | In rapporto all'altezza delle maiuscole |
|---|---|---|
| altezza maiuscole | 504 pt (178 mm, **17,8 % del foglio**) | 1 |
| larghezza B, O, D, S | 142 pt | 0,28 |
| larghezza I (= spessore aste) | 44 pt | **0,087** |
| larghezza E | 117 pt | 0,23 |
| larghezza T | 145 pt | 0,29 |
| larghezza A | 162 pt | 0,32 |
| larghezza L | 110 pt | 0,22 |
| sbordo di O e S sopra/sotto | 7–10 pt | ≈ 1,5 % |
| spazio fra le lettere | ≈ 29 pt | **≈ 6 %** |
| crenatura T–A | −23 pt (si sovrappongono) | — |
| larghezza totale | 1359 pt | **68,5 % del foglio** |

Il disegno è un **grottesco ultracompresso**: rapporto larghezza/altezza 0,28
per le tonde, aste di spessore uniforme (8,7 % dell'altezza), **controforme a
pillola** (fianchi dritti, cappucci semicircolari) in O, D, B, S; la A ha
l'apice tronco piatto; la S è quasi rettilinea. Le lettere sono accostate
quasi a toccarsi.

**Posizione.** Il lettering occupa in orizzontale da x = 340 a x = 1699 pt
(17 %–86 % della larghezza), leggermente spostato a destra del centro (+27 pt).
In verticale la linea di base sta a 756 pt dal bordo alto (26,7 %) e le
maiuscole salgono fino a 252 pt (8,9 %).

### 4.2 Il dispositivo dei due colori

Il lettering è **corallo** (`#FF3852`) sul fondo acqua, e diventa **arancio**
(`#FF6E1C`) esattamente dove attraversa la macchia magenta. Non è una
trasparenza né una fusione: nel file il titolo è disegnato due volte, una in
corallo e una in arancio **ritagliata con il tracciato della macchia** (il
gruppo è marcato in sovrastampa, ma i colori sono piatti). Il risultato è a
tre colori netti, senza toni intermedi.

Procedura per riprodurlo (in qualunque strumento):

1. campire la macchia in magenta;
2. sopra, comporre il titolo in corallo;
3. comporre di nuovo lo **stesso** titolo in arancio, con la macchia usata come
   maschera di ritaglio.

È **la** firma dell'identità: dove testo e forma organica si incontrano, il
testo cambia temperatura invece di sparire o di opacizzarsi.

### 4.3 Carattere sostitutivo

Per comporre parole diverse da "BIODETAILS" (sui font di sistema di questo Mac
nessuna faccia si avvicina) è stato cercato il font libero più vicino,
misurando per ciascun candidato le larghezze di O, I, B, E, T, A, L, S in
rapporto all'altezza delle maiuscole e confrontandole con la tabella sopra:

| Carattere (licenza OFL) | Scarto medio | Note |
|---|---|---|
| **League Gothic**, asse `wdth` = 75 | **0,064** | O 0,27 · I 0,11 · A 0,33 · L 0,23 — praticamente identico |
| Six Caps | 0,245 | ancora più stretto ma aste troppo sottili |
| Antonio Bold | 0,54 | troppo largo |
| Big Shoulders Display Black | 0,89 | troppo largo |
| Saira Extra Condensed Black | 0,90 | troppo largo |

**League Gothic a larghezza 75** è il sostituto. Ha controforme a pillola e
aste uniformi come l'originale; l'unica differenza visibile è l'asta un po' più
spessa (11 % contro 8,7 %). Va composto **tutto maiuscolo, con tracking
minimo** (le lettere quasi a toccarsi), mai in minuscolo. Copre tutti i
caratteri usati da Mind Zoom, accenti e "×" compresi.

---

## 5. La macchia

L'unico elemento non tipografico. Un tracciato chiuso di **40 segmenti cubici**
(estratto e allegato in `macchia.svg` e `biodetails_blob.hpp`).

**Forma.** Tre lobi tondi uniti da colli lisci a S, come un organismo
unicellulare o una molecola vista al microscopio:

- lobo sinistro: piccolo, centrato a ≈ (361 pt, 375 pt dal bordo alto), raggio
  ≈ 213 pt (stima sul render);
- lobo destro: grande, centrato a ≈ (1644, 156), raggio ≈ 298 pt, **sborda
  oltre il margine alto** e vi si ferma (il tracciato è tagliato esattamente
  sulla guida del margine, a 22,7 pt dal bordo);
- lobo inferiore: centrato a ≈ (964, 765), raggio ≈ 234 pt, scende fin sotto il
  lettering;
- i due colli sono un'onda continua che passa **dietro le lettere centrali**
  (O, D, E) all'altezza della loro metà inferiore.

**Riquadro.** 1753 × 992 pt (rapporto 1,767); da x = 114 a 1867 (5,7 %–94,1 %
della larghezza), da y = 23 a 1015 (0,8 %–35,8 % dell'altezza). Occupa quindi
poco più del terzo superiore del foglio.

**Rapporto con il lettering.** La macchia sta **dietro** al titolo (il titolo
resta sempre leggibile per intero) ma **davanti al fondo**; il titolo la taglia
in due orizzontalmente. Le date "9–" in corallo sfiorano il lobo inferiore
senza toccarlo.

**Regole d'uso della macchia** ricavate dal foglio:

- una sola macchia per composizione, sempre magenta pieno, mai contornata;
- può sbordare dal foglio ma **mai dal margine**: si ferma sulla gabbia;
- convive solo con il lettering (che le passa sopra e cambia colore) e con il
  fondo; nessun testo di lettura le passa sopra;
- niente ombre, sfocature, trasparenze.

---

## 6. Segni e procedure grafiche ricorrenti

1. **Campiture piatte, sempre.** Zero sfumature, zero trasparenze, zero
   ombre, zero filetti. La profondità è data solo dalla sovrapposizione di forme
   piatte.
2. **Cambio di colore all'intersezione** (lettering corallo → arancio dentro la
   macchia). È il modo con cui l'identità dice che due cose si toccano.
3. **Numeri come blocchi grafici**: le cifre grandi vanno spezzate su più righe
   e composte con interlinea negativa ("9–" / "11", "20" / "26").
4. **Titoletti arancio Medium + elenco nero Roman**, subito sotto, senza
   distanza: un solo colore d'accento per la gerarchia di secondo livello.
5. **Elenchi di nomi separati da virgola in un unico paragrafo**, mai un nome
   per riga: la densità è voluta.
6. **A capo semantico** sui blocchi a corpo grande: una riga per unità di
   senso.
7. **Un solo valore di spaziatura** (8 mm) per margini e gocce.
8. **Bandiera sinistra** ovunque; blocco giustificato solo per la prosa lunga.

### Cosa NON fare

- Non introdurre grigi, bianchi, toni attenuati o colori fuori palette.
- Non usare i caldi per testi lunghi o piccoli su acqua (contrasto 1,7–2,3).
- Non contornare, sfumare, ombreggiare, arrotondare gli angoli di riquadri.
- Non centrare i blocchi di testo.
- Non usare Bold: il peso massimo è Medium, e serve solo ai titoletti.
- Non ricreare la macchia a mano libera: usare il tracciato allegato.

---

## 7. Traduzione a schermo per Mind Zoom

Mind Zoom è a schermo (proiettore o finestra da 1280 × 800 in su), non su
carta: la scala si trasporta per **proporzione con l'altezza**, non in
punti assoluti. Riferimento: altezza del foglio = altezza della finestra.

### 7.1 Due mondi, non due identità

L'applicazione ha due superfici, e la tavolozza si comporta diversamente su
ciascuna. Sono gli stessi cinque colori, con i ruoli scambiati.

| | **La carta** (schermate di contorno) | **La foto** (esperienza e strumenti) |
|---|---|---|
| dove | ingresso, accoglienza, congedo, calibrazione | immagine al microscopio, pannello operatore, grafici |
| fondo | acqua `#ABCFD5` | **nero** |
| testo | nero | acqua |
| accenti | i tre caldi, solo su corpi grandi | i tre caldi, leggibili a ogni corpo |

Il motivo è misurabile: sull'acqua i tre caldi stanno fra 1,7 e 2,3 di
contrasto, sul nero fra 5,6 e 7,5. Un'etichetta arancione a 12 pt è
illeggibile sull'acqua e perfetta sul nero.

Il fondo dietro la fotografia resta **nero** anche quando l'immagine non
riempie lo schermo: la foto al microscopio ha i propri neri, e una banda color
acqua ai suoi lati la spegnerebbe.

### 7.2 Scala tipografica

| Livello | Sul foglio | A 800 px di altezza | Carattere |
|---|---|---|---|
| lettering di schermata | 17,8 % | ≈ 128 px di maiuscole | League Gothic 75, maiuscolo |
| cifra display (ingrandimento) | 9,9 % | ≈ 44 px | Iowan Old Style, corallo |
| corpo | 0,81 % | → **17-19 px** per leggibilità a distanza | Manrope |
| titoletto | 0,74 % | → 14 px | Manrope, arancio |

I corpi di lettura non possono scendere alla proporzione del poster (un
manifesto si legge da 50 cm, uno schermo in mostra da 2 m): restano quelli già
tarati in Mind Zoom. La proporzione va rispettata invece per i **rapporti** fra
i livelli: lettering ≈ 7 × il corpo.

### 7.3 Che cosa cambia e che cosa no

L'adattamento tocca **colori, lettering delle intestazioni e artefatti**. Non
tocca il resto, per scelta esplicita:

- **I caratteri del testo restano quelli di prima.** Manrope per il corpo,
  Iowan Old Style per cifre ed etichette di servizio. Il carattere nuovo
  (League Gothic) entra **solo** nelle intestazioni di schermata, dove sostituisce
  il lettering disegnato del poster. Helvetica Neue, che nel poster fa tutto,
  qui non entra.
- **Lo stile dei pulsanti resta quello di prima**, costruzione compresa:
  pillola, interno sfocato, ombra interna, bagliore, contorno a sfumatura
  animata. Cambiano solo i colori (magenta profondo al posto del blu notte,
  rampa arancio → corallo → magenta al posto di verde → blu → magenta).
  Questo è l'unico punto in cui l'adattamento si allontana dalla regola
  "campiture piatte" del paragrafo 6, ed è una scelta del progetto: il
  pulsante è l'unico elemento che deve dire "sono premibile".
- **Struttura e contenuti non cambiano.** Stesse schermate, stessa sequenza,
  stessi testi, salvo il paragrafo del respiro quadrato, riscritto più corto.

### 7.4 La macchia, viva

Sulla pagina d'ingresso la macchia non è ferma: sta al centro dello schermo ed
è disegnata come un **campo di metaball**, cioè un campo scalare

    f(p) = Σ r²ᵢ / |p − cᵢ|²,   contorno dove f = 1

con una sorgente per lobo. È la stessa forma del poster letta per quello che
è: tre cerchi uniti da colli lisci sono già un campo di metaball disegnato a
mano. Le sorgenti derivano perciò dai lobi misurati sul tracciato originale
(par. 5), più due satelliti piccoli che rendono più frequenti gli incontri.

Ogni sorgente vaga con la somma di due sinusoidi incommensurabili per asse
(periodi da 29 a 71 secondi): il moto è lento, non si ripete mai e non ha
scatti. Quando due lobi si avvicinano il campo forma da solo il collo, con la
stessa curvatura del poster, e quando si allontanano il collo si assottiglia
e si strappa. La "tensione superficiale" non è simulata: è la forma del campo.

Il campo porta anche un'informazione: **lobi separati** = la fascia non sta
ancora leggendo, **lobi fusi in una massa sola** = segnale presente. È la
stessa cosa che diceva prima lo sciame di puntini, detta con la forma
dell'identità.

Sul congedo la macchia è invece **ferma**, nel tracciato esatto del poster e
nelle sue proporzioni (alta 1,97 volte le maiuscole): lì l'esperienza è finita
e una forma che si agita direbbe il contrario.

Il contorno si ricava con un marching squares su griglia da 6 px, e i pezzi
finiscono in un'unica forma di riempimento perché i bordi in comune si
annullino. Misurato in finestra 1280 × 800 su schermo retina: 60 fps pieni con
celle da 4, 6, 8 e 12 px.

### 7.5 Mappa dei ruoli

| Elemento di Mind Zoom | Prima | Adesso |
|---|---|---|
| fondo delle schermate di contorno | blu notte `#030513` | **acqua `#ABCFD5`** |
| fondo dietro la fotografia | blu notte | **nero** |
| testo di lettura | bianco / azzurro attenuato | **nero** (i toni secondari sono nero a opacità 0,68, mai grigio) |
| intestazioni di schermata | Iowan Old Style corsivo alto | **League Gothic 75, maiuscolo, corallo** |
| corpo, etichette, cifre | Manrope / Iowan Old Style | **invariati** |
| etichette di sezione | verde acido | **arancio** |
| accento "concentrazione" | ciano `#00BFFF` | **magenta `#FF0085`** |
| accento "rilassamento" | verde `#93FF76` | **arancio `#FF6E1C`** |
| pulsanti | pillola blu notte, bordo verde/blu/magenta | **stessa costruzione**, magenta profondo, bordo arancio/corallo/magenta |
| sciame di puntini sull'ingresso | 9000 particelle con dinamica boids | **campo di metaball** dalla macchia del poster |
| badge dell'ingrandimento e scala | pillola scura semitrasparente con ombra | **pillola acqua piatta**, cifre corallo |
| pannello operatore e grafici | blu notte semitrasparente | **nero semitrasparente, testo acqua**, accenti caldi |
| macchia del poster | — | **viva** all'ingresso, **ferma** sul congedo |

### Tre regole per non tradire il foglio

1. **Piatto**, ovunque tranne i pulsanti. Ombre, sfumature e trasparenze del
   vecchio stile vengono tolte, non ricolorate.
2. **Il caldo è per i titoli e i numeri**, e va sul fondo giusto: nero per
   leggere sull'acqua, acqua per leggere sul nero.
3. **La macchia sta dietro il lettering** e lo colora di arancio: se compare,
   compare così.
