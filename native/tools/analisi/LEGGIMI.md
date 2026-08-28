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
