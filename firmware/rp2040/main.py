# Desktop Companion - firmware per RP2040-Zero + OLED SSD1306 128x64
# Riceve dal PC via USB seriale:
#   "S <cpu> <ram> <HH:MM>\n"  stato del PC
#   "A <attività>\n"           Claude: work | ask | done | none
#   "P <scena> [dettaglio]\n"  cosa stai facendo: code | game <gioco> | osm view|edit | none
#   "U <5h%> <sett%> <reset5h> <resetSett>\n"  consumi di Claude ("-" se mancano)
#   "V <gpu%> <temp> <vram%>\n"                GPU
#   "E <umore>\n"              umore di fondo: bored | sleepy | none
#   "M <emozione> <frase>\n"   il robot dice una frase in una nuvoletta
#   "H <1|0|->\n"              in riproduzione: 1 musica, 0 video, - niente
#   "N <titolo>\n" "B <pos> <durata>\n"  titolo e avanzamento (secondi) di musica o video
# Tasto BOOT: breve = pagina successiva (occhi, consumi, orologio), lungo = schermo spento.
import sys, select, time, random, math
from array import array
from machine import Pin, I2C, SoftI2C
import framebuf, rp2, ssd1306, neopixel


def find_display():
    # Cablaggio consigliato: SDA su GP28, SCL su GP29 (I2C0 hardware, più veloce).
    # Se lì non c'è, prova il vecchio cablaggio SDA GP5 / SCL GP4 (I2C software).
    for make in (lambda: I2C(0, sda=Pin(28), scl=Pin(29), freq=400000),
                 lambda: SoftI2C(sda=Pin(5), scl=Pin(4), freq=400000)):
        bus = make()
        if 0x3C in bus.scan():
            return bus
    raise OSError("display OLED non trovato: controlla i collegamenti")


oled = ssd1306.SSD1306_I2C(128, 64, find_display())
led = neopixel.NeoPixel(Pin(16), 1)

EYE_W, EYE_H = 36, 36
EYE_Y = 39
BLINK = (0.6, 0.2, 0.05, 0.2, 0.6)
COLORS = {"sleep": (0, 0, 4), "happy": (0, 8, 0), "normal": (0, 4, 6), "stress": (10, 0, 0),
          "done": (0, 12, 0), "code": (0, 3, 10), "bored": (2, 2, 2), "sleepy": (0, 0, 3),
          "love": (12, 0, 5), "excited": (12, 9, 0), "surprised": (8, 8, 8), "sad": (0, 1, 8)}
ACTIVITIES = ("work", "ask", "done")
SCENES = ("code", "game", "osm", "music", "video")
TIMEOUT_MS = 10000
MSG_MS = 4500  # quanto resta la nuvoletta con la frase

GAME_LABELS = {"minecraft": "MINECRAFT", "sot": "SEA OF THIEVES", "cities": "CITIES",
               "ets2": "EURO TRUCK", "isonzo": "ISONZO", "tlou": "THE LAST OF US"}
OSM_LABELS = {"view": "OPENSTREETMAP", "edit": "OSM: MODIFICA"}

# detail: gioco riconosciuto o modalità di OSM; msg: (emozione, frase, scadenza)
state = {"cpu": 0, "ram": 0, "clock": "--:--", "last": None, "act": "none", "scene": "none",
         "detail": "", "usage": None, "goto": None, "gpu": None, "emo": "none", "msg": None,
         "weather": None, "wtest": None, "music": False, "song": None, "media_pos": None}
buf = ""
poll = select.poll()
poll.register(sys.stdin, select.POLLIN)


def parse(line):
    if line.startswith("M "):  # la frase contiene spazi: va letta a parte
        p = line.split(" ", 2)
        if len(p) == 3:
            state["msg"] = (p[1], p[2], time.ticks_add(time.ticks_ms(), MSG_MS))
        return
    if line.startswith("N "):  # nome del brano (con spazi)
        state["song"] = line[2:]
        return
    p = line.split()
    if len(p) == 4 and p[0] == "S":
        try:
            state["cpu"], state["ram"] = int(p[1]), int(p[2])
        except ValueError:
            return
        state["clock"] = p[3][:5]
        state["last"] = time.ticks_ms()
    elif len(p) == 2 and p[0] == "A":
        state["act"] = p[1]
    elif len(p) in (2, 3) and p[0] == "P":
        state["scene"] = p[1]
        state["detail"] = p[2] if len(p) == 3 else ""
    elif len(p) == 2 and p[0] == "E":
        state["emo"] = p[1]
    elif len(p) == 3 and p[0] == "W":  # meteo: animazione e temperatura
        state["weather"] = p[1] if p[1] != "-" else None
    elif len(p) == 2 and p[0] == "T":  # prova un'animazione meteo adesso (menu sul PC)
        state["wtest"] = p[1]
    elif len(p) == 2 and p[0] == "H":
        state["music"] = p[1] == "1"
        if p[1] == "-":  # non suona più niente
            state["song"] = state["media_pos"] = None
    elif len(p) == 3 and p[0] == "B":
        state["media_pos"] = (int(p[1]), int(p[2]), time.ticks_ms())
    elif len(p) == 5 and p[0] == "U":
        state["usage"] = p[1:]
    elif len(p) == 4 and p[0] == "V":
        state["gpu"] = [int(v) for v in p[1:]] if p[1].isdigit() else None
    elif len(p) == 2 and p[0] == "G":  # il PC chiede di mostrare una pagina
        state["goto"] = int(p[1]) if p[1].isdigit() else 0


def read_serial():
    global buf
    while poll.poll(0):
        c = sys.stdin.read(1)
        if c == "\n":
            parse(buf.strip())
            buf = ""
        else:
            buf += c
            if len(buf) > 64:
                buf = ""


def mood():
    if state["last"] is None or time.ticks_diff(time.ticks_ms(), state["last"]) > TIMEOUT_MS:
        return "sleep"
    if state["act"] in ACTIVITIES:
        return state["act"]
    if state["emo"] == "bored":  # sei lontano dal PC: si annoia anche se c'è VS Code aperto
        return "bored"
    if state["scene"] in SCENES:
        return state["scene"]
    if state["emo"] == "sleepy":
        return "sleepy"
    g = state["gpu"]
    if state["cpu"] >= 80 or (g and (g[0] >= 90 or g[1] >= 80)):
        return "stress"
    if state["cpu"] <= 15:
        return "happy"
    return "normal"


def led_color(m, now):
    if m == "work":  # viola che pulsa
        v = 2 + int(8 * (1 + math.sin(now / 250)) / 2)
        return (v, 0, v)
    if m == "ask":  # arancione lampeggiante
        return (14, 5, 0) if (now // 300) % 2 else (0, 0, 0)
    if m == "osm":
        return (0, 8, 3) if state["detail"] == "view" else (10, 6, 0)
    if m == "music":  # arcobaleno che scorre
        return wheel((now // 15) % 256)
    if m == "video":  # luce azzurrina che tremola, come quella di uno schermo
        return (1, 3, 8) if (now // 180) % 5 else (3, 5, 12)
    if m == "game":
        g = state["detail"]
        if g == "isonzo" and iso["boom"] > 5:  # lampo dell'esplosione
            return (20, 10, 0)
        if g == "tlou" and tlou["lit"]:  # clicker illuminato dalla torcia
            return (14, 0, 0)
        if g in GAME_LED:
            return GAME_LED[g]
        # gioco sconosciuto: rosso e blu alternati, da sala giochi
        t = (1 + math.sin(now / 300)) / 2
        return (int(10 * t), 0, int(10 * (1 - t)))
    return COLORS.get(m, COLORS["normal"])


# ---------------------------------------------------------------- disegno

def rrect(x, y, w, h, r, c=1):
    r = min(r, w // 2, h // 2)
    oled.fill_rect(x + r, y, w - 2 * r, h, c)
    oled.fill_rect(x, y + r, w, h - 2 * r, c)
    for cx, cy in ((x + r, y + r), (x + w - r - 1, y + r),
                   (x + r, y + h - r - 1), (x + w - r - 1, y + h - r - 1)):
        oled.ellipse(cx, cy, r, r, c, True)


def draw_heart(cx, cy, s):
    r = s // 4
    oled.ellipse(cx - r, cy - r // 2, r, r, 1, True)
    oled.ellipse(cx + r, cy - r // 2, r, r, 1, True)
    oled.poly(0, 0, array("h", [cx - 2 * r, cy - r // 3, cx + 2 * r, cy - r // 3, cx, cy + 2 * r]), 1, True)


def draw_eye(cx, cy, w, h, m, side):
    x, y = cx - w // 2, cy - h // 2
    if m == "love" and h > 10:  # occhi a cuore
        draw_heart(cx, cy, w)
        return
    if m == "surprised" and h > 10:  # occhi tondi spalancati con la pupilla
        oled.ellipse(cx, cy, w // 2, h // 2, 1, True)
        oled.ellipse(cx, cy, w // 6, w // 6, 0, True)
        return
    rrect(x, y, w, h, 8 if w > 24 else 5)
    if h <= 10:
        return
    if m == "work":  # super concentrato: occhi stretti e sopracciglia aggrottate verso il centro
        oled.fill_rect(x, y, w, h * 3 // 10, 0)
        tri = array("h", [0, 0, w, 0, w, h * 6 // 10]) if side < 0 else array("h", [0, 0, w, 0, 0, h * 6 // 10])
        oled.poly(x, y, tri, 0, True)
    elif m == "bored":  # palpebre pesanti e dritte
        oled.fill_rect(x, y, w, h * 55 // 100, 0)
    elif m in ("sad", "sleepy"):  # palpebra che scende verso l'esterno
        tri = array("h", [0, 0, w, 0, 0, h * 6 // 10]) if side < 0 else array("h", [0, 0, w, 0, w, h * 6 // 10])
        oled.poly(x, y, tri, 0, True)
        if m == "sleepy":
            oled.fill_rect(x, y, w, h * 35 // 100, 0)
    elif m in ("happy", "done", "excited"):
        # taglia la parte bassa con un'ellisse: occhio ad arco ^
        oled.ellipse(cx, y + h + 6, w // 2 + 4, h // 2 + 2, 0, True)
    elif m in ("stress", "game"):
        # palpebra inclinata verso il centro: sguardo concentrato
        tri = array("h", [0, 0, w, 0, w, h // 2]) if side < 0 else array("h", [0, 0, w, 0, 0, h // 2])
        oled.poly(x, y, tri, 0, True)
    elif m == "sleep":
        oled.fill_rect(x, y, w, h // 2 + 4, 0)
    elif m == "ask":
        # pupilla: occhi spalancati che ti guardano
        oled.ellipse(cx, cy, 6, 6, 0, True)


def center_text(s, y):
    oled.text(s, (128 - 8 * len(s)) // 2, y)


def draw_thought(now):
    # nuvoletta dei pensieri in alto a destra, con i puntini che compaiono
    oled.ellipse(92, 30, 2, 2, 1, True)
    oled.ellipse(99, 22, 3, 3, 1, True)
    for cx, cy, r in ((106, 11, 7), (116, 8, 8), (123, 13, 6), (113, 15, 7)):
        oled.ellipse(cx, cy, r, r, 1, True)
    for i in range((now // 350) % 4):
        oled.fill_rect(105 + i * 6, 11, 3, 3, 0)
    # consumi di Claude in alto a sinistra, mentre lavora
    u = state["usage"]
    if u and u[:2] != ["-", "-"]:
        if u[0] != "-":
            oled.text("5h " + u[0] + "%", 0, 0)
        if u[1] != "-":
            oled.text("sett " + u[1] + "%", 0, 10)


# righe di codice (rientro, lunghezza) che scorrono sullo schermo del portatile
code_lines = [(random.randint(0, 3) * 4, random.randint(8, 36)) for _ in range(5)]
typed = 0


def draw_laptop(now):
    global typed, code_lines
    # schermo
    oled.rect(30, 28, 68, 29, 1)
    oled.rect(31, 29, 66, 27, 1)
    # base/tastiera
    rrect(22, 57, 84, 5, 2)
    oled.hline(56, 59, 16, 0)
    u = state["usage"]
    if u and u[:2] != ["-", "-"] and (now // 5000) % 2:  # ogni 5 s: consumi di Claude sullo schermo
        oled.text("5h " + (u[0] + "%" if u[0] != "-" else "--"), 33, 33)
        oled.text("sett" + (u[1] + "%" if u[1] != "-" else "--"), 33, 45)
        return
    # codice: tutte le righe complete tranne l'ultima, che si sta scrivendo
    for i, (indent, length) in enumerate(code_lines):
        n = length if i < len(code_lines) - 1 else min(typed, length)
        oled.hline(35 + indent, 33 + i * 5, n, 1)
        oled.hline(35 + indent, 34 + i * 5, n, 1)
    indent, length = code_lines[-1]
    # cursore lampeggiante
    if (now // 250) % 2:
        oled.vline(35 + indent + min(typed, length) + 1, 32 + (len(code_lines) - 1) * 5, 4, 1)
    typed += random.choice((0, 1, 2, 2, 3))
    if typed > length + 6:  # riga finita: va a capo e scorre
        code_lines = code_lines[1:] + [(random.randint(0, 3) * 4, random.randint(8, 36))]
        typed = 0


pressed = [0, 0, 0, 0, 0]  # 4 tasti + d-pad


def draw_gamepad(now):
    # corpo e impugnature
    rrect(34, 36, 60, 20, 8)
    oled.ellipse(40, 55, 8, 8, 1, True)
    oled.ellipse(88, 55, 8, 8, 1, True)
    # croce direzionale (si "sposta" quando viene premuta)
    dx = pressed[4]
    oled.fill_rect(42 + dx, 43, 12, 4, 0)
    oled.fill_rect(46 + dx, 39, 4, 12, 0)
    # 4 tasti: buco pieno a riposo, solo contorno quando premuti
    for i, (bx, by) in enumerate(((80, 40), (86, 45), (80, 50), (74, 45))):
        oled.ellipse(bx, by, 2, 2, 0, not pressed[i])
    # tasti premuti a caso, come in partita
    if random.random() < 0.35:
        i = random.randint(0, 4)
        pressed[i] = random.choice((-1, 1)) if i == 4 else 1 - pressed[i]


# ---------------------------------------------------------------- scene dei giochi
# Il companion in miniatura (testolina con gli occhioni) dentro la scena del gioco.
# Il nome del gioco sta in alto (y 0..7), la scena usa y 10..63.

face = {"k": 1.0, "look": 0}  # palpebre e sguardo, aggiornati dal ciclo principale


def draw_buddy(x, y, w, h, look=0, squint=False):
    """Il companion in miniatura: i suoi due occhioni, dentro il riquadro x,y,w,h."""
    ew = max(4, w * 3 // 10)
    full = h - 6
    eh = max(1, int(full * face["k"]))
    if squint:
        eh = max(1, eh // 3)
    ey = y + h // 2 - eh // 2 + 1
    dx = max(-2, min(2, look + face["look"] // 3))
    for cx in (x + w // 4 + 1, x + 3 * w // 4 - 1):
        ex = cx - ew // 2 + dx
        oled.fill_rect(ex - 1, ey - 1, ew + 2, eh + 2, 0)  # bordino nero: staccano dallo sfondo
        rrect(ex, ey, ew, eh, 3)


# Minecraft: il companion scava un blocco di terra col piccone
DIRT = ((3, 7), (10, 6), (6, 10), (13, 11), (2, 13), (9, 14), (12, 8), (5, 13), (1, 9), (14, 14))
CRACKS = ((8, 8, 5, 11), (8, 8, 12, 6), (5, 11, 2, 10), (8, 8, 11, 13), (12, 6, 14, 3), (5, 11, 6, 15))
SWING = (-80, -80, -60, -15, -45, -70)  # angoli del piccone, il 4° è il colpo
mc = {"hits": 0, "phase": -1, "debris": 0}


def draw_minecraft(now):
    bx, by = 74, 44
    phase = (now // 110) % len(SWING)
    if phase == 3 and mc["phase"] != 3 and not mc["debris"]:
        mc["hits"] += 1
        if mc["hits"] > 4:
            mc["hits"], mc["debris"] = 0, 6
    mc["phase"] = phase

    oled.hline(0, 60, 128, 1)
    if mc["debris"]:  # frammenti che volano via
        d = 6 - mc["debris"]
        for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):
            oled.fill_rect(bx + 7 + dx * (3 + d * 3), by + 7 + dy * (3 + d * 2) + d * d // 3, 3, 3, 1)
        mc["debris"] -= 1
    else:
        oled.fill_rect(bx, by, 16, 16, 1)
        oled.hline(bx, by + 4, 16, 0)  # confine erba/terra
        for i in range(0, 16, 3):
            oled.pixel(bx + i, by + 5, 0)
        for px, py in DIRT:
            oled.pixel(bx + px, by + py, 0)
        for x1, y1, x2, y2 in CRACKS[:mc["hits"] + mc["hits"] // 2]:
            oled.line(bx + x1, by + y1, bx + x2, by + y2, 0)

    draw_buddy(12, 33, 32, 27, look=2, squint=phase == 3)
    # piccone impugnato "a mezz'aria" accanto agli occhi
    px, py = 50, 50
    a = math.radians(SWING[phase])
    ca, sa = math.cos(a), math.sin(a)
    ex, ey = px + 24 * ca, py + 24 * sa
    for o in (0, 1):  # manico spesso 2 pixel
        oled.line(px + o, py, int(ex) + o, int(ey), 1)
    for s in (-1, 1):  # testa ad arco: le punte si piegano verso il manico
        mx, my = ex + s * 5 * -sa, ey + s * 5 * ca
        tx, ty = ex + s * 10 * -sa - 3 * ca, ey + s * 10 * ca - 3 * sa
        for o in (-1, 0, 1):
            oled.line(int(ex), int(ey) + o, int(mx), int(my) + o, 1)
        oled.line(int(mx), int(my), int(tx), int(ty), 1)
        oled.line(int(mx), int(my) + 1, int(tx), int(ty) + 1, 1)
    if phase == 3:  # scintille del colpo
        for sx, sy in ((73, 41), (72, 46), (74, 38)):
            oled.pixel(sx, sy, 1)


# Sea of Thieves: il companion sbuca dal bordo del galeone che dondola
SKULL = ((1, 0), (2, 0), (3, 0), (0, 1), (2, 1), (4, 1), (1, 2), (2, 2), (3, 2), (1, 3), (3, 3))
HULL = array("h", [-32, 0, 32, 0, 24, 9, -24, 9])


def draw_ship(now):
    t = now / 220
    bob = int(2 * math.sin(now / 380))
    cx, base = 64, 50 + bob
    # albero, vela con teschio, bandiera
    mx = cx + 10
    oled.vline(mx, base - 38, 38, 1)
    oled.fill_rect(mx - 11, base - 34, 23, 15, 1)
    for sx, sy in SKULL:
        oled.pixel(mx - 2 + sx, base - 29 + sy, 0)
    f = (now // 180) % 2
    oled.line(mx, base - 38, mx + 7, base - 37 - f, 1)
    oled.line(mx, base - 37, mx + 7, base - 36 - f, 1)
    # il companion sul ponte, lo scafo gli copre la parte bassa
    draw_buddy(cx - 30, base - 22, 28, 28, look=-1)
    oled.poly(cx, base, HULL, 1, True)
    oled.hline(cx - 26, base + 3, 52, 0)  # linea dei cannoni
    for gx in range(cx - 20, cx + 24, 9):
        oled.fill_rect(gx, base + 2, 3, 3, 0)
    # onde
    for x in range(0, 128, 2):
        oled.fill_rect(x, 60 + int(2 * math.sin(x / 7 + t)), 2, 1, 1)
        oled.pixel(x + 1, 63 - int(math.sin(x / 5 - t)), 1)


# Cities: Skylines: il companion col caschetto guarda la città che cresce
city = {"b": [], "done_at": None}


def new_city():
    b, x = [], 44
    while x < 120:
        w = random.randint(7, 11)
        b.append([x, w, random.randint(8, 26), 0.0])
        x += w + 2
    city["b"], city["done_at"] = b, None


def draw_city(now):
    if not city["b"]:
        new_city()
    growing = None
    for bl in city["b"]:
        if bl[3] < bl[2]:
            growing = bl
            break
    if growing:
        growing[3] = min(growing[2], growing[3] + 0.4)
    elif city["done_at"] is None:
        city["done_at"] = now
    elif time.ticks_diff(now, city["done_at"]) > 3000:
        new_city()
        return
    oled.hline(0, 63, 128, 1)
    for x, w, target, h in city["b"]:
        h = int(h)
        if not h:
            continue
        top = 63 - h
        oled.fill_rect(x, top, w, h, 1)
        for wy in range(top + 2, 61, 3):  # finestre che si accendono e spengono
            for wx in range(x + 2, x + w - 1, 3):
                if (wx * 7 + wy * 13 + now // 900) % 4:
                    oled.pixel(wx, wy, 0)
    if growing:  # gru sul palazzo in costruzione
        x, w, _, h = growing
        gx, top = x + w // 2, 63 - int(h)
        oled.vline(gx, top - 9, 9, 1)
        oled.hline(gx - 5, top - 9, 16, 1)
        oled.vline(gx + 9, top - 8, 3 + int(2 * math.sin(now / 300)), 1)
    # companion con il caschetto da cantiere
    bx, by = 6, 38
    draw_buddy(bx, by, 30, 25, look=2)
    oled.ellipse(bx + 15, by + 1, 12, 7, 1, True, 0b0011)
    oled.hline(bx - 2, by + 1, 34, 1)
    oled.vline(bx + 15, by - 5, 5, 0)  # cresta del caschetto


# Euro Truck Simulator: il companion guida il camion, lo si vede dal finestrino
def draw_truck(now):
    # alberi sullo sfondo che scorrono all'indietro
    for i in range(4):
        tx = 140 - (now // 30 + i * 42) % 168
        oled.vline(tx, 18, 5, 1)
        oled.ellipse(tx, 15, 4, 4, 1, True)
    # strada con il tratteggio che corre
    oled.hline(0, 61, 128, 1)
    for x in range(-((now // 15) % 16), 128, 16):
        oled.hline(x, 63, 8, 1)
    v = (now // 120) % 2  # vibrazione del motore
    # rimorchio
    oled.fill_rect(4, 24 + v, 66, 28, 1)
    oled.rect(7, 27 + v, 60, 22, 0)
    # cabina con muso
    rrect(72, 28 + v, 30, 25, 4)
    oled.fill_rect(98, 38 + v, 10, 15, 1)
    oled.fill_rect(104, 42 + v, 2, 6, 0)  # griglia
    # finestrino con gli occhioni del companion che guardano la strada
    rrect(78, 31 + v, 20, 13, 3, 0)
    eh = max(1, int(8 * face["k"]))
    for ex in (84, 91):
        rrect(ex + 1, 33 + v + (8 - eh) // 2, 4, eh, 1)
    # tubo di scarico e fumo
    oled.vline(73, 18 + v, 10, 1)
    for k in range(3):
        p = (now // 90 + k * 5) % 15
        oled.ellipse(71 - p, 16 - p // 3, 1 + p // 5, 1 + p // 5, 1, False)
    # ruote che girano
    spin = (now // 60) % 4
    for wx in (16, 30, 58, 82, 100):
        oled.ellipse(wx, 55, 5, 5, 1, True)
        oled.ellipse(wx, 55, 2, 2, 0, True)
        dx, dy = ((0, -4), (4, 0), (0, 4), (-4, 0))[spin]
        oled.pixel(wx + dx, 55 + dy, 0)


# Isonzo: il companion con l'elmetto in trincea, esplosioni sulle montagne
MOUNTAINS = array("h", [0, 40, 18, 26, 30, 34, 50, 20, 70, 36, 88, 24, 104, 33, 127, 25, 127, 40])
iso = {"boom": 0, "x": 0, "y": 0}


def draw_trench(now):
    oled.poly(0, 0, MOUNTAINS, 1, False)
    oled.line(50, 20, 45, 24, 1)  # neve sulle cime
    oled.line(50, 20, 55, 24, 1)
    oled.line(88, 24, 84, 27, 1)
    oled.line(88, 24, 92, 27, 1)
    oled.hline(0, 45, 128, 1)  # filo spinato e cavalli di frisia
    for x in range(2, 128, 14):
        oled.line(x, 42, x + 6, 48, 1)
        oled.line(x + 6, 42, x, 48, 1)
    if not iso["boom"] and random.random() < 0.03:
        iso["boom"], iso["y"] = 9, random.randint(26, 36)
        iso["x"] = random.choice((random.randint(10, 36), random.randint(92, 118)))
    if iso["boom"]:
        r = 10 - iso["boom"]
        bx, by = iso["x"], iso["y"]
        if iso["boom"] > 6:  # lampo
            oled.ellipse(bx, by, r * 2, r * 2, 1, True)
        else:  # nuvola che si allarga e detriti
            oled.ellipse(bx, by, r * 2, r * 2, 0, True)
            oled.ellipse(bx, by, r * 2, r * 2, 1, False)
            for dx, dy in ((-3, -2), (3, -3), (-2, 1), (4, 0)):
                oled.pixel(bx + dx * r, by + dy * r // 2, 1)
        iso["boom"] -= 1
    # il companion con l'elmetto sbuca dalla trincea (trema durante le esplosioni)
    shake = random.randint(-1, 1) if iso["boom"] > 3 else 0
    bx, by = 49 + shake, 36
    draw_buddy(bx, by, 30, 24, look=0, squint=iso["boom"] > 0)
    oled.ellipse(bx + 15, by + 2, 14, 8, 1, True, 0b0011)
    oled.hline(bx - 4, by + 2, 38, 1)
    oled.hline(bx - 4, by + 3, 38, 1)
    for x in range(0, 128, 11):  # sacchi di sabbia davanti
        oled.ellipse(x + 5, 60, 5, 3, 1, True)
        oled.ellipse(x + 5, 60, 5, 3, 0, False)
    oled.fill_rect(0, 63, 128, 1, 1)


# The Last of Us: rovine al buio, gli occhioni con la torcia; si vede solo ciò che il fascio
# illumina. Ogni tanto nel buio c'è un clicker: se la luce lo becca, gli occhi si spaventano.
RUINS = ((44, 30, 18, 33), (66, 22, 14, 41), (84, 34, 20, 29), (108, 26, 18, 37))
tlou = {"lit": False, "next": 0, "last": None, "scare_until": 0}
zombies = []  # [tipo, x, seme per l'animazione]; si avvicinano da destra verso gli occhi
ZOMBIE_SPEED = {"runner": 0.045, "clicker": 0.012}  # pixel al millisecondo


def draw_runner(x, now, seed):
    """Runner: corre verso sinistra, busto in avanti, braccia e gambe che si alternano."""
    step = (now // 90 + seed) % 2
    oled.ellipse(x - 1, 41, 3, 3, 1, True)
    oled.line(x, 44, x + 2, 53, 1)  # busto piegato in avanti
    oled.line(x + 1, 44, x + 3, 53, 1)
    if step:
        oled.line(x, 46, x - 7, 49, 1)
        oled.line(x + 1, 46, x + 6, 43, 1)
        oled.line(x + 2, 53, x - 4, 62, 1)
        oled.line(x + 3, 53, x + 8, 60, 1)
    else:
        oled.line(x, 46, x - 6, 43, 1)
        oled.line(x + 1, 46, x + 6, 50, 1)
        oled.line(x + 2, 53, x + 6, 62, 1)
        oled.line(x + 3, 53, x - 2, 59, 1)


def draw_clicker(x, now, seed, lit):
    """Clicker: cammina lento, testa a fungo che scatta; se illuminato fa "click"."""
    hx = x + (random.randint(-1, 1) if (now // 120 + seed) % 3 == 0 else 0)
    oled.ellipse(hx, 34, 4, 5, 1, True)
    for dx, dy in ((-5, -4), (5, -4), (-6, 1), (6, 2), (0, -6)):  # placche del fungo
        oled.line(hx, 34, hx + dx, 34 + dy, 1)
    oled.fill_rect(x - 3, 39, 7, 12, 1)
    oled.line(x - 3, 41, x - 9, 48, 1)  # braccia protese
    oled.line(x + 3, 41, x - 2, 46, 1)
    sway = (now // 250 + seed) % 2
    oled.line(x - 2, 51, x - 4 + sway * 2, 62, 1)
    oled.line(x + 2, 51, x + 4 - sway * 2, 62, 1)
    if lit and (now // 180) % 2:  # onde del "click"
        oled.ellipse(hx + 5, 34, 3, 4, 1, False, 0b1001)
        oled.ellipse(hx + 5, 34, 6, 7, 1, False, 0b1001)


def draw_tlou(now):
    for x, y, w, h in RUINS:  # palazzi in rovina, finestre rotte e rampicanti
        oled.rect(x, y, w, h, 1)
        oled.line(x, y, x + w // 3, y - 5, 1)
        oled.line(x + w // 3, y - 5, x + w // 2, y + 2, 1)
        for wy in range(y + 4, y + h - 4, 7):
            for wx_ in range(x + 3, x + w - 3, 5):
                oled.fill_rect(wx_, wy, 2, 3, 1)
        for i in range(2):
            vx = x + 3 + i * (w - 6)
            for vy in range(y, y + h * 2 // 3, 2):
                oled.pixel(vx + int(1.5 * math.sin(vy / 3 + i)), vy, 1)
    oled.hline(36, 62, 92, 1)
    oled.rect(50, 55, 20, 5, 1)  # macchina abbandonata
    oled.line(54, 55, 57, 51, 1)
    oled.hline(57, 51, 7, 1)
    oled.line(64, 51, 67, 55, 1)
    oled.ellipse(54, 60, 2, 2, 1, True)
    oled.ellipse(66, 60, 2, 2, 1, True)
    # zombie: ne arrivano fino a due, runner veloci e clicker lenti
    dt = 0 if tlou["last"] is None else max(0, min(100, time.ticks_diff(now, tlou["last"])))
    tlou["last"] = now
    if len(zombies) < 2 and time.ticks_diff(now, tlou["next"]) > 0:
        zombies.append([random.choice(("runner", "runner", "clicker")), 134.0, random.randint(0, 9)])
        tlou["next"] = time.ticks_add(now, random.randint(1500, 4500))
    for z in zombies[:]:
        z[1] -= ZOMBIE_SPEED[z[0]] * dt
        if z[1] < 46:  # è arrivato agli occhi: sparisce, e che spavento!
            zombies.remove(z)
            tlou["scare_until"] = time.ticks_add(now, 700)
    # fascio della torcia: calcolato prima per sapere chi è illuminato
    ox, oy = 38, 48
    a, half = -0.15 + 0.32 * math.sin(now / 1400), 0.34
    lit = False
    for kind, zx, seed in zombies:
        z_lit = abs(math.atan2(40 - oy, zx - ox) - a) < half
        lit = lit or z_lit
        oled.fill_rect(int(zx) - 10, 27, 20, 35, 0)  # sagoma staccata dalle rovine
        if kind == "runner":
            draw_runner(int(zx), now, seed)
        else:
            draw_clicker(int(zx), now, seed, z_lit)
    # tutto ciò che sta fuori dal cono torna al buio
    t1, t2 = math.tan(a - half), math.tan(a + half)
    for x in range(ox, 128, 2):
        top, bot = oy + int((x - ox) * t1), oy + int((x - ox) * t2)
        if top > 10:
            oled.fill_rect(x, 10, 2, top - 10, 0)
        if bot < 64:
            oled.fill_rect(x, max(10, bot), 2, 64 - max(10, bot), 0)
    scared = time.ticks_diff(tlou["scare_until"], now) > 0
    tlou["lit"] = lit or scared
    for i in range(10):  # spore che fluttuano, visibili solo nella luce
        sx = 40 + (i * 29 + now // 70) % 88
        sy = 12 + (i * 17 + int(4 * math.sin(now / 600 + i))) % 50
        if abs(math.atan2(sy - oy, sx - ox) - a) < half:
            oled.pixel(sx, sy, 1)
    # gli occhioni al buio, con la torcia in mano
    oled.fill_rect(0, 10, ox, 54, 0)
    jump = -4 if scared else 0  # salto di spavento
    draw_buddy(0, 34 + jump, 28, 28, look=1, squint=lit or scared)
    oled.fill_rect(30, 46 + jump, 8, 5, 1)  # torcia
    oled.vline(38, 45 + jump, 7, 1)
    if (lit or scared) and (now // 150) % 2:
        oled.text("!!" if scared else "!", 28, 26)


GAME_SCENES = {"minecraft": draw_minecraft, "sot": draw_ship, "cities": draw_city,
               "ets2": draw_truck, "isonzo": draw_trench, "tlou": draw_tlou}
GAME_LED = {"minecraft": (0, 10, 0), "sot": (0, 6, 8), "cities": (10, 8, 0), "ets2": (12, 4, 0),
            "isonzo": (3, 1, 0), "tlou": (0, 3, 1)}


# ---------------------------------------------------------------- OpenStreetMap
# Consultazione: gli occhi esplorano la mappa dentro una lente d'ingrandimento.
# Modifica: gli occhi disegnano un edificio nodo per nodo, come nell'editor iD.

ROADS = ((0, 46, 128, 32), (26, 10, 40, 63), (88, 10, 100, 63), (0, 60, 128, 55))
BLOCKS = ((48, 14, 12, 8), (106, 40, 14, 9), (6, 26, 12, 10), (56, 50, 14, 8))


def draw_map(blocks=True):
    for x1, y1, x2, y2 in ROADS:
        oled.line(x1, y1, x2, y2, 1)
    for x in range(0, 128, 2):  # fiume
        oled.pixel(x, 21 + int(3 * math.sin(x / 10)), 1)
    if blocks:
        for x, y, w, h in BLOCKS:
            oled.rect(x, y, w, h, 1)


def draw_osm_view(now):
    draw_map()
    # segnaposto che rimbalza
    px, py = 104, 24 - abs(int(3 * math.sin(now / 200)))
    oled.poly(0, 0, array("h", [px - 4, py + 1, px + 4, py + 1, px, py + 9]), 1, True)
    oled.ellipse(px, py, 4, 4, 1, True)
    oled.ellipse(px, py, 1, 1, 0, True)
    # lente che gira per la mappa, con gli occhioni dentro
    t = now / 1600
    lx, ly = 52 + int(26 * math.sin(t)), 38 + int(9 * math.sin(t * 1.7))
    for o in range(3):  # manico
        oled.line(lx + 10 + o, ly + 10, lx + 17 + o, ly + 17, 1)
    oled.ellipse(lx, ly, 14, 14, 0, True)
    oled.ellipse(lx, ly, 14, 14, 1, False)
    oled.ellipse(lx, ly, 13, 13, 1, False)
    draw_buddy(lx - 11, ly - 10, 22, 20, look=int(2 * math.cos(t)))


BUILDINGS = (((72, 22), (104, 22), (104, 36), (92, 36), (92, 50), (72, 50)),
             ((66, 20), (100, 20), (100, 48), (66, 48)),
             ((68, 32), (86, 18), (106, 32), (106, 52), (68, 52)))
osm_edit = {"shape": 0, "n": 1, "step": -1}


def draw_osm_edit(now):
    draw_map(blocks=False)
    pts = BUILDINGS[osm_edit["shape"]]
    step = now // 450
    if step != osm_edit["step"]:  # ogni 450 ms aggiunge un nodo; chiuso l'edificio, ne inizia un altro
        osm_edit["step"] = step
        osm_edit["n"] += 1
        if osm_edit["n"] > len(pts) + 5:
            osm_edit["shape"] = (osm_edit["shape"] + 1) % len(BUILDINGS)
            osm_edit["n"] = 1
            pts = BUILDINGS[osm_edit["shape"]]
    n = osm_edit["n"]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    oled.fill_rect(min(xs) - 3, min(ys) - 3, max(xs) - min(xs) + 7, max(ys) - min(ys) + 7, 0)
    closed = n > len(pts)
    flat = array("h", [c for p in pts for c in p])
    if closed:  # edificio finito: area riempita a righe, come un'area selezionata
        oled.poly(0, 0, flat, 1, False)
        for y in range(min(ys) + 2, max(ys), 3):
            for x in range(min(xs) + 2, max(xs), 2):
                if (x + y) % 4 == 0:
                    oled.pixel(x, y, 1)
    placed = pts[:min(n, len(pts))]
    for (x1, y1), (x2, y2) in zip(placed, placed[1:] + (pts[:1] if closed else ())):
        oled.line(x1, y1, x2, y2, 1)
        oled.line(x1, y1 + 1, x2, y2 + 1, 1)
    for x, y in placed:  # nodi: quadratini bianchi col centro nero
        oled.fill_rect(x - 2, y - 2, 5, 5, 1)
        oled.pixel(x, y, 0)
    # matita: dall'ultimo nodo va verso il prossimo, tirando la linea "elastica"
    if closed:
        tx, ty = pts[0]
    else:
        (x1, y1), (x2, y2) = pts[n - 1], pts[n % len(pts)]
        f = (now % 450) / 450
        tx, ty = int(x1 + (x2 - x1) * f), int(y1 + (y2 - y1) * f)
        oled.line(x1, y1, tx, ty, 1)
    for o in range(3):
        oled.line(tx + 2 + o, ty - 2, tx + 12 + o, ty - 12, 1)
    oled.line(tx + 3, ty - 3, tx + 13, ty - 13, 0)
    oled.pixel(tx, ty, 1)
    oled.pixel(tx + 1, ty - 1, 1)
    # gli occhioni, in basso a sinistra, guardano l'edificio
    oled.fill_rect(0, 32, 44, 32, 0)
    draw_buddy(4, 34, 36, 28, look=2)


OSM_SCENES = {"view": draw_osm_view, "edit": draw_osm_edit}


# ---------------------------------------------------------------- musica
# Cuffie in testa e occhioni che ballano a tempo, note che escono dalle cuffie e il titolo
# del brano che scorre in basso.

BEAT_MS = 500  # circa 120 battiti al minuto
DANCE = ("normal", "happy", "normal", "love")  # espressione che cambia ogni 4 battiti
notes = []  # [x, y, velocità orizzontale]
music_fx = {"last": None, "next_note": 0}


def wheel(pos):
    """Colore dell'arcobaleno (0-255), a bassa luminosità."""
    if pos < 85:
        return (12 - pos * 12 // 85, pos * 12 // 85, 0)
    if pos < 170:
        pos -= 85
        return (0, 12 - pos * 12 // 85, pos * 12 // 85)
    pos -= 170
    return (pos * 12 // 85, 0, 12 - pos * 12 // 85)


def draw_note(x, y):
    oled.ellipse(x, y, 2, 2, 1, True)
    oled.vline(x + 2, y - 7, 7, 1)
    oled.line(x + 2, y - 7, x + 5, y - 4, 1)


def draw_headphones(cx, top, half_w, cup_y, cup_h):
    oled.ellipse(cx, cup_y, half_w, cup_y - top, 1, False, 0b0011)  # archetto
    oled.ellipse(cx, cup_y, half_w - 1, cup_y - top - 1, 1, False, 0b0011)
    for x, inner in ((cx - half_w - 4, 5), (cx + half_w - 3, 2)):  # padiglioni
        rrect(x, cup_y - 2, 8, cup_h, 3)
        oled.vline(x + inner, cup_y + 1, cup_h - 6, 0)


def draw_music(now):
    beat = (now % BEAT_MS) / BEAT_MS
    n = now // BEAT_MS
    bounce = -int(5 * math.sin(math.pi * beat))  # saltello a ogni battito
    ox = int(7 * math.sin(now * math.pi / (2 * BEAT_MS)))  # dondolio a destra e sinistra
    cy = 30 + bounce
    h = 22 if beat < 0.12 else 26  # si schiaccia sul battito
    h = max(2, int(h * face["k"]))
    m = DANCE[(n // 4) % len(DANCE)]
    for cx, side in ((47 + ox, -1), (81 + ox, 1)):
        draw_eye(cx, cy, 25, h, m, side)
    draw_headphones(64 + ox, cy - 22, 36, cy - 6, 16)
    # note che escono dalle cuffie e salgono
    dt = 0 if music_fx["last"] is None else max(0, min(100, time.ticks_diff(now, music_fx["last"])))
    music_fx["last"] = now
    if time.ticks_diff(now, music_fx["next_note"]) > 0 and len(notes) < 6:
        left = random.random() < 0.5
        notes.append([24 + ox if left else 104 + ox, cy, -0.012 if left else 0.012])
        music_fx["next_note"] = time.ticks_add(now, random.randint(400, 900))
    for note in notes[:]:
        note[1] -= 0.02 * dt
        note[0] += note[2] * dt
        if note[1] < 8:
            notes.remove(note)
        else:
            draw_note(int(note[0]), int(note[1]))
    draw_media_footer(now)


def fmt_time(s):
    return "%d:%02d" % (s // 60, s % 60)


def draw_media_footer(now):
    """In basso: barra del tempo (trascorso e durata) e titolo che scorre."""
    b = state["media_pos"]
    if b and b[1] > 0:
        pos, dur, at = b
        cur = min(dur, pos + max(0, time.ticks_diff(now, at)) // 1000)  # avanza anche tra un aggiornamento e l'altro
        left, right = fmt_time(cur), fmt_time(dur)
        oled.fill_rect(0, 45, 128, 10, 0)
        oled.text(left, 0, 46)
        oled.text(right, 128 - 8 * len(right), 46)
        x0, x1 = 8 * len(left) + 3, 128 - 8 * len(right) - 3
        oled.rect(x0, 47, x1 - x0, 5, 1)
        oled.fill_rect(x0 + 1, 48, (x1 - x0 - 2) * cur // dur, 3, 1)
    song = state["song"]
    if song:
        span = len(song) * 8 + 128
        oled.fill_rect(0, 55, 128, 9, 0)
        oled.text(song, 128 - (now // 35) % span, 56)


def draw_popcorn(x, y):
    oled.poly(0, 0, array("h", [x, y, x + 18, y, x + 15, y + 20, x + 3, y + 20]), 1, True)  # secchiello
    for i in range(3):  # strisce
        oled.line(x + 4 + i * 4, y + 2, x + 5 + i * 3, y + 18, 0)
    for dx, dy in ((2, -1), (7, -3), (12, -2), (16, -1), (9, 0)):  # popcorn che trabocca
        oled.ellipse(x + dx, y + dy, 3, 3, 1, True)
        oled.pixel(x + dx, y + dy, 0)


def draw_video(now):
    """Guarda un video: occhi rivolti allo schermo (cioè verso di te), con la luce che
    tremola, e il secchiello dei popcorn con qualche chicco che salta fuori."""
    flicker = 2 if (now // 180) % 5 == 0 else 0
    h = max(2, int((22 - flicker) * face["k"]))
    dx = max(-3, min(3, face["look"] // 3))
    for cx, side in ((38, -1), (72, 1)):
        draw_eye(cx + dx, 22, 24, h, "normal", side)
    draw_popcorn(100, 18)
    t = (now % 1400) / 1400  # chicco che salta con una parabola
    oled.ellipse(109 + int(12 * t), 14 - int(28 * t - 30 * t * t), 2, 2, 1, True)
    draw_media_footer(now)


# ---------------------------------------------------------------- frasi (nuvoletta)

def wrap(text, width=15):
    lines, cur = [], ""
    for word in text.split():
        if cur and len(cur) + 1 + len(word) > width:
            lines.append(cur)
            cur = word
        else:
            cur = (cur + " " + word).strip()
    lines.append(cur)
    return [l[:width] for l in lines[:2]]


def draw_speech(emo, text, k, now):
    rrect(0, 0, 128, 25, 6)
    rrect(2, 2, 124, 21, 5, 0)
    oled.poly(0, 0, array("h", [54, 23, 70, 23, 57, 32]), 1, True)  # codina della nuvoletta
    oled.poly(0, 0, array("h", [57, 21, 67, 21, 58, 28]), 0, True)
    lines = wrap(text)
    for i, l in enumerate(lines):
        center_text(l, (8 if len(lines) == 1 else 4 + i * 9))
    bounce = -abs(int(2 * math.sin(now / 140))) if emo in ("excited", "love", "happy") else 0
    h = max(2, int(24 * k))
    for cx, side in ((44, -1), (84, 1)):
        draw_eye(cx, 48 + bounce, 26, h, emo, side)


def top_bar(m, now):
    if m == "sleep":
        n = (now // 500) % 4
        for i in range(n):
            oled.text("z", 104 + i * 7, 12 - i * 5)
    elif m == "ask":
        if (now // 400) % 2:
            center_text("Ti cerco!", 0)
    elif m == "done":
        center_text("Fatto!", 0)
    elif m in ("work", "code", "game", "osm", "music", "video"):
        pass  # la scena riempie lo schermo
    else:
        # la riga in alto ruota ogni 4 s: ora e CPU, GPU, consumi di Claude
        u, g = state["usage"], state["gpu"]
        phase = (now // 4000) % 3
        if phase == 1 and g:
            oled.text("GPU %d%%" % g[0], 0, 0)
            s = "%dC" % g[1]
            oled.text(s, 128 - 8 * len(s), 0)
        elif phase == 2 and u and u[:2] != ["-", "-"]:
            left = "5h %s%%" % u[0] if u[0] != "-" else ""
            right = "sett %s%%" % u[1] if u[1] != "-" else ""
            oled.text(left, 0, 0)
            oled.text(right, 128 - 8 * len(right), 0)
        else:
            oled.text(state["clock"], 0, 0)
            s = "CPU%3d%%" % state["cpu"]
            oled.text(s, 128 - 8 * len(s), 0)


# ---------------------------------------------------------------- meteo (animazione ogni tanto)
# Non è una pagina: ogni 6-10 minuti, se il robot non sta facendo altro, per 8 secondi
# reagisce al tempo che fa: trema, suda, apre l'ombrello, mette gli occhiali da sole...

WEATHER_ANIM_MS = 8000
WEATHER_EVERY = (6 * 60000, 10 * 60000)
WEATHER_LED = {"cold": (0, 4, 12), "hot": (14, 4, 0), "rain": (0, 2, 10), "snow": (8, 8, 10),
               "storm": (4, 0, 8), "sun": (14, 10, 0), "moon": (2, 2, 6), "fog": (3, 3, 3),
               "cloud": (4, 5, 6)}
wx = {"kind": None, "until": 0, "next": None, "forced": False, "inverted": False}


def sweat_drop(x, y):
    oled.ellipse(x, y, 2, 2, 1, True)
    oled.poly(0, 0, array("h", [x - 2, y - 1, x + 2, y - 1, x, y - 6]), 1, True)


def draw_sun(now):
    oled.ellipse(117, 9, 5, 5, 1, True)
    for i in range(8):  # raggi che girano
        a = i * 0.785 + now / 1500
        oled.line(117 + int(7 * math.cos(a)), 9 + int(7 * math.sin(a)),
                  117 + int(10 * math.cos(a)), 9 + int(10 * math.sin(a)), 1)


def draw_umbrella(now):
    oled.ellipse(64, 18, 46, 14, 1, True, 0b0011)  # tela
    for x in range(25, 110, 13):  # bordo a festoni
        oled.ellipse(x, 18, 6, 3, 0, True, 0b0011)
    oled.vline(64, 1, 4, 1)  # puntale
    oled.fill_rect(64, 18, 2, 40, 1)  # manico, passa tra gli occhi
    oled.ellipse(60, 58, 5, 4, 1, False, 0b1100)
    for i, x in enumerate((3, 9, 15, 113, 119, 125, 30, 48, 80, 98)):
        if 18 < x < 110:  # sopra l'ombrello le gocce si fermano sulla tela
            y = (now // 25 + i * 17) % 12 - 8
        else:
            y = (now // 25 + i * 17) % 70 - 6
        oled.vline(x, y, 4, 1)


def draw_sunglasses(ox):
    for cx in (36 + ox, 92 + ox):
        rrect(cx - 21, 24, 42, 26, 7)
        rrect(cx - 19, 26, 38, 22, 6, 0)  # lente scura
        oled.line(cx - 13, 40, cx - 5, 31, 1)  # riflessi
        oled.line(cx - 9, 43, cx - 2, 35, 1)
    oled.fill_rect(57 + ox, 29, 14, 2, 1)  # ponte
    oled.hline(0, 28, 15 + ox, 1)  # stanghette
    oled.hline(113 + ox, 28, 15 - ox, 1)


def draw_weather(kind, now, k, lx):
    """Occhi + effetti per l'animazione meteo `kind`."""
    ox = int(lx) // 2
    if kind == "sun":
        draw_sunglasses(ox)
        draw_sun(now)
        return
    m, h, oy = "normal", EYE_H, 0
    if kind == "cold":  # trema
        ox += random.choice((-2, 2))
        h = EYE_H * 85 // 100
    elif kind == "hot":
        m = "sleepy"
    elif kind == "fog":
        h = EYE_H // 2
    elif kind == "storm":
        m = "surprised"
    elif kind == "snow":
        m = "happy"
    elif kind == "cloud":
        oy = -3
    h = max(2, int(h * k))
    for cx, side in ((36, -1), (92, 1)):
        draw_eye(cx + ox, EYE_Y + oy, EYE_W, h, m, side)

    if kind in ("rain", "storm"):
        draw_umbrella(now)
        if kind == "storm" and now % 1800 < 120:  # fulmine
            oled.poly(0, 0, array("h", [10, 0, 18, 0, 13, 9, 19, 9, 7, 26, 10, 13, 5, 13]), 1, True)
    elif kind == "cold":
        if (now // 400) % 2:
            center_text("Brrr!", 0)
        for x in (5, 121):  # tremolio ai lati
            for y in range(26, 52, 5):
                oled.line(x - 1, y, x + 1, y + 2, 1)
                oled.line(x + 1, y + 2, x - 1, y + 4, 1)
    elif kind == "hot":
        draw_sun(now)
        for x, phase in ((12, 0), (116, 12)):  # gocce di sudore che scendono
            sweat_drop(x, 24 + (now // 45 + phase) % 26)
        shift = (now // 150) % 8  # aria che tremola in basso
        for x in range(0, 128, 4):
            oled.hline(x, 61 + (1 if (x // 4 + shift) % 4 < 2 else 0), 2, 1)
    elif kind == "snow":
        for i in range(14):
            x = (i * 37 + int(6 * math.sin(now / 500 + i))) % 128
            y = (now // 45 + i * 29) % 72 - 4
            for dx, dy in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                oled.pixel(x + dx, y + dy, 1)
    elif kind == "moon":
        oled.ellipse(114, 11, 9, 9, 1, True)
        oled.ellipse(118, 8, 8, 8, 0, True)
        for i, (sx, sy) in enumerate(((8, 6), (30, 13), (62, 4), (88, 12), (100, 58), (20, 58))):
            if (now // 300 + i) % 3:
                oled.pixel(sx, sy, 1)
                if (now // 300 + i) % 3 == 1:
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        oled.pixel(sx + dx, sy + dy, 1)
    elif kind == "fog":  # banchi di nebbia che scorrono, anche davanti agli occhi
        for r, y in enumerate((18, 29, 41, 52)):
            off = (now // 60) % 14 * (1 if r % 2 else -1)
            for x in range(-14 + off, 128, 14):
                over_eye = 21 <= y <= 57 and (18 <= x <= 54 or 74 <= x <= 110)
                oled.hline(x, y, 8, 0 if over_eye else 1)
    elif kind == "cloud":  # nuvola che attraversa lo schermo in alto
        left = WEATHER_ANIM_MS - time.ticks_diff(wx["until"], now)
        cx = 150 - left * 190 // WEATHER_ANIM_MS
        for dx, dy, r in ((0, 10, 7), (9, 6, 8), (18, 10, 7), (9, 12, 6)):
            oled.ellipse(cx + dx, dy, r, r, 1, True)


def weather_led(kind, now):
    if kind == "storm" and now % 1800 < 120:
        return (16, 16, 16)
    return WEATHER_LED.get(kind, COLORS["normal"])


# ---------------------------------------------------------------- pagine (tasto BOOT)

PAGE_TIMEOUT_MS = 20000  # dopo quanto si torna da soli agli occhi
LONG_PRESS_MS = 800
btn = {"down": None, "long": False}


def read_button(now):
    """Restituisce "short", "long" o None. Il tasto BOOT si legge con rp2.bootsel_button()."""
    pressed = rp2.bootsel_button()
    if pressed and btn["down"] is None:
        btn["down"], btn["long"] = now, False
    elif pressed and not btn["long"] and time.ticks_diff(now, btn["down"]) > LONG_PRESS_MS:
        btn["long"] = True
        return "long"  # scatta mentre è ancora premuto, così si sente la risposta subito
    elif not pressed and btn["down"] is not None:
        was_long, held = btn["long"], time.ticks_diff(now, btn["down"])
        btn["down"] = None
        if not was_long and held > 30:
            return "short"
    return None


def big_text(s, x, y, scale):
    w = 8 * len(s)
    fb = framebuf.FrameBuffer(bytearray(w), w, 8, framebuf.MONO_VLSB)
    fb.text(s, 0, 0, 1)
    for py in range(8):
        for px in range(w):
            if fb.pixel(px, py):
                oled.fill_rect(x + px * scale, y + py * scale, scale, scale, 1)


def usage_row(y, title, pct, reset):
    p = "--" if pct == "-" else pct + "%"
    oled.text(title, 0, y)
    oled.text(p, 128 - 8 * len(p), y)
    oled.rect(0, y + 10, 128, 7, 1)
    if pct != "-":
        oled.fill_rect(2, y + 12, min(124, 124 * int(pct) // 100), 3, 1)
    if reset != "-":
        oled.text("reset " + reset.replace("_", " "), 0, y + 20)


def draw_usage_page(now):
    u = state["usage"]
    if not u or u[:2] == ["-", "-"]:
        center_text("CONSUMI CLAUDE", 12)
        center_text("nessun dato:", 32)
        center_text("usa Claude Code", 44)
        return
    usage_row(0, "5 ore", u[0], u[2])
    usage_row(33, "Settimana", u[1], u[3])


def draw_clock_page(now):
    clock = state["clock"]
    if (now // 500) % 2:
        clock = clock.replace(":", " ")  # due punti che lampeggiano
    big_text(clock, 4, 0, 3)
    rows = [("CPU", state["cpu"], "%d%%" % state["cpu"]), ("RAM", state["ram"], "%d%%" % state["ram"])]
    g = state["gpu"]
    if g:  # a destra alterna uso e temperatura
        rows.append(("GPU", g[0], "%d%%" % g[0] if (now // 3000) % 2 else "%dC" % g[1]))
    for i, (name, val, txt) in enumerate(rows):
        y = 27 + i * 12
        oled.text(name, 0, y)
        oled.rect(28, y, 66, 8, 1)
        oled.fill_rect(30, y + 2, 62 * min(val, 100) // 100, 4, 1)
        oled.text(txt, 128 - 8 * len(txt), y)


PAGES = (None, draw_usage_page, draw_clock_page)  # 0 = occhi


def splash():
    oled.fill(0)
    oled.text("Ciao!", 44, 28)
    oled.show()
    time.sleep_ms(1200)


# ---------------------------------------------------------------- ciclo principale

def main():
    splash()
    now = time.ticks_ms()
    blink_at, blink_i = time.ticks_add(now, 2000), -1
    look_at, lx, ly, tx, ty = now, 0.0, 0.0, 0, 0
    last_mood, last_led = None, None
    page, page_until, screen_off = 0, now, False

    while True:
        read_serial()
        now = time.ticks_ms()
        m = mood()
        if m != last_mood:
            look_at = now  # nuovo umore: riposiziona subito lo sguardo
            last_mood = m
        msg = state["msg"]
        if msg and time.ticks_diff(now, msg[2]) > 0:
            state["msg"] = msg = None
        speaking = msg is not None and m not in ("ask", "sleep")

        # tasto BOOT e pagine
        ev = read_button(now)
        if screen_off and (ev or m == "ask"):  # qualsiasi pressione (o Claude che ti cerca) riaccende
            screen_off, ev, page = False, None, 0
            oled.poweron()
        if ev == "long":
            screen_off = True
            oled.fill(0)
            oled.show()
            oled.poweroff()
        elif ev == "short":
            page = (page + 1) % len(PAGES)
            page_until = time.ticks_add(now, PAGE_TIMEOUT_MS)
        if state["goto"] is not None:  # pagina richiesta dal menu sul PC
            page, state["goto"] = state["goto"] % len(PAGES), None
            page_until = time.ticks_add(now, PAGE_TIMEOUT_MS)
            if screen_off:
                screen_off = False
                oled.poweron()
        if page and (m == "ask" or time.ticks_diff(now, page_until) > 0):
            page = 0

        # meteo: ogni tanto un'animazione, solo quando il robot non sta facendo altro
        idle_mood = m in ("normal", "happy", "stress") and not speaking and not page
        if state["wtest"]:
            wx["kind"], wx["until"], wx["forced"] = state["wtest"], time.ticks_add(now, WEATHER_ANIM_MS), True
            state["wtest"] = None
        elif idle_mood and state["weather"] and wx["kind"] is None:
            if wx["next"] is None:
                wx["next"] = time.ticks_add(now, 20000)  # la prima volta poco dopo l'avvio
            elif time.ticks_diff(now, wx["next"]) > 0:
                wx["kind"], wx["until"], wx["forced"] = state["weather"], time.ticks_add(now, WEATHER_ANIM_MS), False
                wx["next"] = time.ticks_add(now, random.randint(*WEATHER_EVERY))
        if wx["kind"] and time.ticks_diff(now, wx["until"]) > 0:
            wx["kind"] = None
        weather_on = (wx["kind"] is not None and not screen_off and not page and not speaking
                      and m not in ("ask", "sleep") and (wx["forced"] or idle_mood))
        flash = weather_on and wx["kind"] == "storm" and now % 1800 < 120
        if flash != wx["inverted"]:  # lampo del temporale: schermo invertito per un attimo
            oled.invert(1 if flash else 0)
            wx["inverted"] = flash

        if screen_off:
            c = (0, 0, 0)
        elif weather_on:
            c = weather_led(wx["kind"], now)
        else:
            c = led_color(msg[0] if speaking else m, now)
        if c != last_led:
            led[0] = c
            led.write()
            last_led = c

        # battito di palpebre (quasi mai quando ti cerca)
        k = 1.0
        if blink_i < 0 and time.ticks_diff(now, blink_at) >= 0:
            blink_i = 0
        if blink_i >= 0:
            k = BLINK[blink_i]
            blink_i += 1
            if blink_i >= len(BLINK):
                blink_i = -1
                wait = random.randint(4000, 8000) if m == "ask" else random.randint(1500, 5000)
                blink_at = time.ticks_add(now, wait)

        # sguardo
        if time.ticks_diff(now, look_at) >= 0:
            if m == "sleep":
                tx, ty, wait = 0, 3, 3000
            elif m == "work":  # sguardo fisso e intenso, con piccoli scatti
                tx, ty, wait = 4 + random.randint(-1, 1), -1 + random.randint(-1, 1), random.randint(150, 400)
            elif m == "code":  # legge le righe sullo schermo
                tx, ty, wait = random.randint(-4, 4), 2, random.randint(400, 1200)
            elif m == "game":  # sguardo che scatta, reattivo
                tx, ty, wait = random.randint(-5, 5), random.randint(-1, 2), random.randint(150, 500)
            elif m == "ask":
                tx, ty, wait = 0, 0, 3000
            elif m == "bored":  # sguardo lento da una parte all'altra
                tx, ty, wait = (7 if tx <= 0 else -7), 2, random.randint(3000, 5000)
            elif m == "sleepy":
                tx, ty, wait = 0, 3, 3000
            else:
                tx, ty = random.randint(-8, 8), random.randint(-4, 4)
                wait = random.randint(1200, 4000)
            look_at = time.ticks_add(now, wait)
        speed = 0.6 if m == "game" else 0.25
        lx += (tx - lx) * speed
        ly += (ty - ly) * speed

        if screen_off:
            time.sleep_ms(30)
            continue
        oled.fill(0)
        if page:
            PAGES[page](now)
            oled.show()
            time.sleep_ms(30)
            continue
        if speaking:
            draw_speech(msg[0], msg[1], k, now)
            oled.show()
            time.sleep_ms(30)
            continue
        if weather_on:
            draw_weather(wx["kind"], now, k, lx)
            oled.show()
            time.sleep_ms(30)
            continue
        # forma e posizione degli occhi per ogni scena
        if m == "ask":
            w, h, eyes, ey = 40, 44, (36, 92), EYE_Y
        elif m == "work":
            w, h, eyes, ey = 32, 30, (26, 66), 40
        elif m in SCENES:
            w, h, eyes, ey = 24, 18, (46, 82), 13
        else:
            w, h, eyes, ey = EYE_W, EYE_H, (36, 92), EYE_Y
        h = max(2, int(h * k))
        if m == "sleep":
            h = max(2, int(h * 0.6))
        ox, oy = int(lx), int(ly)
        if m in SCENES:  # occhi piccoli: movimenti più contenuti
            ox, oy = ox // 2, max(-2, min(2, oy))
        if m == "stress":
            ox += random.randint(-1, 1)
        elif m == "ask":
            oy += int(2 * math.sin(now / 120))  # saltella
        elif m == "done":
            oy -= abs(int(4 * math.sin(now / 150)))  # saltella di gioia
        face["k"], face["look"] = k, int(lx)
        # scene con gli occhioni dentro (giochi conosciuti, OpenStreetMap)
        scene_fn = None
        if m == "game":
            scene_fn = GAME_SCENES.get(state["detail"])
        elif m == "osm":
            scene_fn = OSM_SCENES.get(state["detail"])
        elif m == "music":
            scene_fn = draw_music
        elif m == "video":
            scene_fn = draw_video
        if not scene_fn:
            for cx, side in ((eyes[0], -1), (eyes[1], 1)):
                draw_eye(cx + ox, ey + oy, w, h, m, side)

        if m == "work":
            draw_thought(now)
            if now % 5000 < 1500:  # goccia di sudore per lo sforzo
                sweat_drop(6, 30 + (now % 5000) // 120)
        elif m == "code":
            draw_laptop(now)
            if state["music"]:  # programmi ascoltando musica: le cuffie in testa
                draw_headphones(64 + ox, 1, 36, 10 + oy, 12)
        elif m == "game":
            (scene_fn or draw_gamepad)(now)
            label = GAME_LABELS.get(state["detail"])
            if label:
                center_text(label, 0)
        elif m in ("music", "video"):
            scene_fn(now)
        elif scene_fn:
            scene_fn(now)
            center_text(OSM_LABELS[state["detail"]], 0)
        top_bar(m, now)
        oled.show()
        time.sleep_ms(30)


main()
