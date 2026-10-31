# Desktop Companion

Un compagno da scrivania che reagisce a quello che fai al PC: Claude Code, VS Code, giochi, musica, video, OpenStreetMap, meteo...

Funziona con tre dispositivi, anche collegati insieme:

- **Robot RP2040**: RP2040-Zero + OLED SSD1306 0.96" monocromatico (firmware MicroPython in `firmware/rp2040`)
- **Arduino Nesso N1**: schermo a colori 135×240, pagina **COMPANION** dentro la Nesso N1 Suite (firmware in `firmware/nesso_n1`)
- **Adafruit MacroPad RP2040**: il robot sullo schermo, più 12 tasti e la manopola per audio, microfono e app (vedi [MacroPad](#macropad))

## Installazione (Windows)

Apri **PowerShell** e incolla:

```powershell
irm https://raw.githubusercontent.com/AndreaDp271/Companion-Robot/main/install.ps1 | iex
```

Installa Python se manca, scarica l'app in `%LOCALAPPDATA%\DesktopCompanion`, installa le librerie, la fa partire con Windows, crea il collegamento **Desktop Companion** sul desktop (per riavviarla) e la avvia. Poi collega il robot o il Nesso N1 via USB.

Se l'antivirus blocca il comando (alcuni bloccano qualsiasi comando PowerShell che scarica da internet): scarica la repo con **Code → Download ZIP**, estraila, fai clic destro su `install.ps1` → **Esegui con PowerShell**.

Il Nesso N1 senza app sul PC mostra un QR code che porta qui; se è associato al PC via Bluetooth, tenendo premuto KEY2 apre questa pagina direttamente sul PC.

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

È la prima pagina della Suite (KEY1 scorre le altre come prima) ed è l'unica in orizzontale. Occhioni a tutto schermo che cambiano colore con l'umore, accessori a colori (cuffie arcobaleno, popcorn, icone dei giochi, mappa OSM, codice colorato), titolo e barra del tempo di musica e video in basso.

- **Rotazione automatica**: girando il Nesso sottosopra lo schermo si raddrizza da solo (accelerometro), in orizzontale sulla pagina COMPANION e in verticale sulle altre. Se il verso è sbagliato, dal monitor seriale: `#rot l -1` (orizzontale) o `#rot p -1` (verticale), resta salvato
- **KEY2 breve**: vista successiva, occhi → consumi Claude → orologio con CPU/RAM/GPU e meteo (dopo 20 s torna agli occhi)
- **KEY2 lungo**: con l'app sul PC chiede di aggiornare tutto da GitHub; senza app apre questa pagina sul PC via tastiera Bluetooth (va associato "Nesso N1" nel Bluetooth di Windows; se era già associato come mouse, rimuovilo e riassocialo una volta) oppure mostra il QR

## Il personaggio

Il companion ha due occhioni, due sopracciglia e una bocca: l'umore si legge da tutti e tre (sorriso, "o" di stupore, linguetta quando è concentrato, sbadiglio quando si annoia, canta con la musica). Ogni scena è disegnata per il Nesso a colori e, più semplice, per l'OLED in bianco e nero. Quando lavora Claude Code al suo posto c'è la mascotte arancione.

Le scene (giochi, musica, video, OpenStreetMap, meteo, feste) sono in `firmware/nesso_n1/NessoN1_Suite/Scenes.ino` e in `firmware/rp2040/main.py`; l'anteprima animata con tutte le scene è stata fatta come pagina web.

## Cosa mostra

| Situazione | Robot | LED |
|---|---|---|
| Claude sta lavorando | al posto degli occhioni c'è la mascotte di Claude Code: cammina a occhi stretti, con la stellina che gira e un verbo ("Rimugino...", "Cucino...") | viola pulsante |
| Claude ti chiede qualcosa | la mascotte saltella e saluta, "?" e "Ti cerco!" | arancione lampeggiante |
| Claude ha finito | la mascotte salta di gioia tra le scintille, "Fatto!" | verde |
| VS Code in primo piano | programma al portatile | blu |
| Musica in riproduzione (Spotify, video musicali su YouTube, ...) | cuffie in testa, occhi che ballano, note, barra del tempo e titolo che scorre | arcobaleno |
| Video normale (YouTube, ...) | guarda il video coi popcorn, barra del tempo e titolo | azzurro che tremola |
| Musica mentre programmi | scena di VS Code, con le cuffie | blu |
| osm.org nel browser | esplora la mappa con la lente | verde |
| editor di OSM (iD su osm.org, o JOSM) | disegna un edificio nodo per nodo | arancione |
| Lontano dal PC 5 minuti | annoiato, palpebre pesanti | grigio |
| Notte (00:30-5:00) | assonnato | blu scuro |
| Gioco a schermo intero o controller in uso | una scena per gioco (Minecraft, Sea of Thieves, Cities: Skylines, Euro Truck, Isonzo, The Last of Us), altrimenti un giochino a piattaforme sul monitor | rosso/blu |
| Normale | in alto ruotano ora e CPU, GPU e temperatura, consumi Claude; umore in base a CPU e GPU | verde / azzurro / rosso |
| PC spento o app chiusa | dorme | blu tenue |

## Meteo

Scegli la città dal menu (**Cambia città...**). L'app legge il meteo da Open-Meteo ogni 15 minuti. Ogni 6-10 minuti, se il robot non sta facendo altro, per 8 secondi reagisce al tempo: trema col freddo (sotto i 5 °C), suda col caldo (da 29 °C), apre l'ombrello se piove, lampi col temporale, fiocchi con la neve, occhiali da sole col sereno, luna e stelle di notte, nebbia, nuvole. Quando il tempo cambia lo commenta. Per provarle: **Prova animazione → Meteo**.

## Feste

Nei giorni di festa l'animazione del meteo si alterna con una a tema, e il robot ti fa gli auguri appena sei al PC (e ogni tanto ne parla nelle chiacchiere):

| Festa | Quando | Animazione |
|---|---|---|
| Capodanno | 31 dicembre, 1 gennaio | fuochi d'artificio e cappellino da festa |
| Befana | 6 gennaio | la Befana sulla scopa attraversa il cielo |
| San Valentino | 14 febbraio | occhi a cuore e cuoricini che salgono |
| Carnevale | dal giovedì al martedì grasso | mascherina e coriandoli |
| Pasqua | Pasqua e Pasquetta | orecchie da coniglio e ovetti colorati |
| Festa della Repubblica e della Liberazione | 25 aprile, 2 giugno | tricolore che sventola |
| Ferragosto | 15 agosto | occhiali da sole e pallone da spiaggia |
| Halloween | 31 ottobre | cappello da strega, zucche, pipistrelli |
| Natale | dall'8 dicembre al 5 gennaio | cappello di Babbo Natale, neve, alberello |

Le date sono in `pc/feasts.py`. Per provarle: **Prova animazione → Feste**.

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

## MacroPad

Sull'Adafruit MacroPad RP2040 gira lo stesso firmware del robot (`firmware/rp2040/main.py`) più `firmware/macropad/board.py`, che gestisce display SH1106, LED, tasti ed encoder. Lo speaker resta sempre spento. L'audio passa da AudioTray (`%USERPROFILE%\AudioTray`, comandi UDP su `127.0.0.1:47801`), così compare il suo OSD; se AudioTray non è aperto, l'app lo avvia.

Prima installazione: MicroPython per Raspberry Pi Pico (tieni premuta la manopola, premi reset, copia l'UF2 sul disco RPI-RP2), poi:

```
python -m mpremote cp firmware/rp2040/main.py :main.py + cp firmware/macropad/board.py :board.py + reset
```

L'app lo riconosce da solo (manda `!hello macropad`) e da lì in poi lo aggiorna con **Riscrivi firmware sui dispositivi**.

| | | |
|---|---|---|
| **Audio** muto | **Microfono** on/off | **CastelSafe** |
| **Indietro** | **Play/Pausa** | **Avanti** |
| **VS Code** | **Brave** | **Esplora file** |
| **Discord** | **Telegram** | **Companion**: pagina occhi/consumi/orologio, tenuto = schermo spento |

- **Luci**: ogni tasto ha il suo colore fisso; Audio e Microfono diventano rossi quando sono muti
- **Indietro/Avanti**: brano precedente/successivo; se il player non ne ha (un video singolo su YouTube) salta di 10 secondi
- **Manopola**: gira = volume, clic = casse ↔ cuffie, gira tenendola premuta = volume del microfono, tenuta = pannello di AudioTray
- **Schermo**: in basso, sotto gli occhi, uscita attiva con volume e stato del microfono
- **Casse ↔ cuffie**: sulla Sound Blaster Z AudioTray cambia l'uscita della scheda (come Sound Blaster Command) e ricorda un volume separato per casse e cuffie; con altre schede passa al dispositivo audio successivo di Windows
- **Tasti delle app**: portano l'app in primo piano (se è già davanti passa alla finestra dopo o la riduce a icona; se non è aperta la avvia). Tenuti premuti: nuova finestra
- I tasti si cambiano in `config.json` → `"macropad": {"keys": [...]}`, stesso formato di `KEYS` in `pc/macropad.py`

## Installazione manuale

```
pip install -r pc/requirements.txt
pythonw "Desktop Companion.pyw"
```
