# Desktop Companion

Un compagno da scrivania che reagisce a quello che fai al PC: Claude Code, VS Code, giochi, musica, video, OpenStreetMap, meteo...

Funziona con due dispositivi, anche collegati insieme:

- **Robot RP2040**: RP2040-Zero + OLED SSD1306 0.96" monocromatico (firmware MicroPython in `firmware/rp2040`)
- **Arduino Nesso N1**: schermo a colori 135×240, pagina **COMPANION** dentro la Nesso N1 Suite (firmware in `firmware/nesso_n1`)

## Installazione (Windows)

Apri **PowerShell** e incolla:

```powershell
irm https://raw.githubusercontent.com/AndreaDp271/Companion-Robot/main/install.ps1 | iex
```

Installa Python se manca, scarica l'app in `%LOCALAPPDATA%\DesktopCompanion`, installa le librerie, la fa partire con Windows e la avvia. Poi collega il robot o il Nesso N1 via USB.

Il Nesso N1 senza app sul PC mostra un QR code che porta qui.

## Aggiornamenti

L'app controlla questa repo all'avvio e ogni 12 ore (`version.json`). Se c'è una versione nuova avvisa, e dal menu **Aggiornamenti → Aggiorna ora** scarica e installa:

- l'**app** (i tuoi file `config.json` e il registro restano), poi si riavvia da sola
- il **firmware del robot RP2040** (`firmware/rp2040/main.py`, copiato con mpremote)
- il **firmware del Nesso N1** (`firmware/nesso_n1/build/NessoN1_Suite.ino.bin`, scritto con esptool solo nella zona dell'app: le impostazioni salvate sul Nesso restano)

Per pubblicare una nuova versione: aggiorna i file, alza il numero in `version.json` (`app`, `rp2040` o `nesso`) e fai push.

### Nesso N1: compilare il firmware

Board `esp32:esp32:arduino_nesso_n1`, librerie elencate in cima a `NessoN1_Suite.ino`. Le credenziali (Wi-Fi, chiave OpenWeatherMap) vanno in `secrets.h` (copia `secrets.example.h`), che **non** va su GitHub. Il binario pubblicato qui è compilato senza credenziali: il Wi-Fi si imposta dalla pagina SETUP via Bluetooth.

## Uso

Compare un'icona vicino all'orologio (se non la vedi, guarda nella freccia `^` delle icone nascoste). Col tasto destro trovi:

- **Stato**: dispositivi collegati, cosa sta facendo Claude, cosa stai facendo tu, consumi, meteo
- **Prova animazione**: per vedere tutte le espressioni
- **Mostra sul robot**: pagine consumi / orologio
- **Cambia città...**: per il meteo
- **Chiacchiere**, **Reagisci a Claude Code**, **Avvia con Windows**
- **Aggiornamenti**: controlla e installa da GitHub, riscrive il firmware sui dispositivi
- **Apri registro**: log in caso di problemi

## Nesso N1: pagina COMPANION

È la prima pagina della Suite (KEY1 scorre le altre come prima). Occhioni colorati che cambiano colore con l'umore, accessori a colori (cuffie arcobaleno, popcorn, icone dei giochi, mappa OSM, codice colorato) e sotto un pannello con consumi Claude, CPU/RAM/GPU, meteo e brano in riproduzione. Bip quando Claude ti cerca. KEY2 mostra il QR della repo.

## Cosa mostra

| Situazione | Robot | LED |
|---|---|---|
| Claude sta lavorando | super concentrato (occhi stretti, sopracciglia aggrottate, goccia di sudore) e nuvoletta | viola pulsante |
| Claude ti chiede qualcosa | "Ti cerco!", occhi spalancati | arancione lampeggiante |
| Claude ha finito | "Fatto!", saltella | verde |
| VS Code in primo piano | programma al portatile | blu |
| Musica in riproduzione (Spotify, video musicali su YouTube, ...) | cuffie in testa, occhi che ballano, note, barra del tempo e titolo che scorre | arcobaleno |
| Video normale (YouTube, ...) | guarda il video coi popcorn, barra del tempo e titolo | azzurro che tremola |
| Musica mentre programmi | scena di VS Code, con le cuffie | blu |
| osm.org nel browser | esplora la mappa con la lente | verde |
| editor di OSM (iD su osm.org, o JOSM) | disegna un edificio nodo per nodo | arancione |
| Lontano dal PC 5 minuti | annoiato, palpebre pesanti | grigio |
| Notte (00:30-5:00) | assonnato | blu scuro |
| Gioco a schermo intero o controller in uso | gioca col gamepad | rosso/blu |
| Normale | in alto ruotano ora e CPU, GPU e temperatura, consumi Claude; umore in base a CPU e GPU | verde / azzurro / rosso |
| PC spento o app chiusa | dorme | blu tenue |

## Meteo

Scegli la città dal menu (**Cambia città...**). L'app legge il meteo da Open-Meteo ogni 15 minuti. Ogni 6-10 minuti, se il robot non sta facendo altro, per 8 secondi reagisce al tempo: trema col freddo (sotto i 5 °C), suda col caldo (da 29 °C), apre l'ombrello se piove, lampi col temporale, fiocchi con la neve, occhiali da sole col sereno, luna e stelle di notte, nebbia, nuvole. Quando il tempo cambia lo commenta. Per provarle: **Prova animazione → Meteo**.

## Frasi

Il robot dice qualcosa in una nuvoletta in base a cosa fai: quando apri un gioco, VS Code o OSM, quando torni al PC dopo una pausa, al mattino e a pranzo, se è tardi, dopo 90 minuti senza pause, dopo un'ora di codice, se CPU o GPU scottano, se i token di Claude stanno finendo, e ogni tanto per chiacchierare. Le frasi sono in `pc/personality.py`: modificale o aggiungine. Le chiacchiere a caso si spengono dal menu (**Chiacchiere**).

## Tasto BOOT

- **Pressione breve**: cambia pagina, occhi → consumi Claude → orologio con CPU/RAM → occhi. Dopo 20 secondi torna da solo agli occhi.
- **Pressione lunga** (1 s): non disturbare, schermo e LED spenti. Si riaccende con una pressione, o da solo se Claude ti cerca.

## Consumi di Claude

L'app legge i consumi delle 5 ore e della settimana (gli stessi di `/usage`) ogni 5 minuti, e subito dopo ogni risposta di Claude. Usa il login che Claude Code salva sul PC (`~/.claude/.credentials.json`): il token viene mandato solo ad `api.anthropic.com`. Funziona anche con l'estensione di VS Code. Il servizio non è documentato, quindi un giorno potrebbe cambiare.

Nel terminale, la barra di stato di Claude Code (`pc/statusline.py`) mostra gli stessi consumi e li manda anche lei al robot.

## Collegamenti

| OLED | RP2040-Zero |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SDA | GP28 |
| SCL | GP29 |

(Il firmware accetta anche il vecchio collegamento SDA GP5 / SCL GP4.)

## Installazione manuale

```
pip install -r pc/requirements.txt
pythonw "Desktop Companion.pyw"
```
