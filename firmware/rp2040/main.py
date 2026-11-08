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
# Sull'Adafruit MacroPad c'è anche board.py (firmware/macropad): display, LED, tasti ed encoder.
import sys, select, time, random, math
from array import array
from machine import Pin, I2C, SoftI2C
import framebuf, rp2, neopixel

try:
    import board
except ImportError:
    board = None


def find_display():
    # Cablaggio consigliato: SDA su GP28, SCL su GP29 (I2C0 hardware, più veloce).
    # Se lì non c'è, prova il vecchio cablaggio SDA GP5 / SCL GP4 (I2C software).
    for make in (lambda: I2C(0, sda=Pin(28), scl=Pin(29), freq=400000),
                 lambda: SoftI2C(sda=Pin(5), scl=Pin(4), freq=400000)):
        bus = make()
        if 0x3C in bus.scan():
            return bus
    raise OSError("display OLED non trovato: controlla i collegamenti")


if board:
    oled, led = board.oled, board.led
else:
    import ssd1306
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
         "weather": None, "feast": None, "wtest": None, "music": False, "song": None, "media_pos": None}
buf = ""
poll = select.poll()
poll.register(sys.stdin, select.POLLIN)


def parse(line):
    if board and board.parse(line):
        return
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
    elif len(p) == 2 and p[0] == "F":  # oggi è festa (o "-")
        state["feast"] = p[1] if p[1] != "-" else None
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


def center_text(s, y):
    oled.text(s, (128 - 8 * len(s)) // 2, y)


# ---------------------------------------------------------------- Claude Code: la mascotte
# Quando Claude lavora, ti cerca o ha finito, al posto degli occhioni c'è il granchietto del
# terminale di Claude Code (18 x 10 unità: corpo, braccini, quattro zampette, occhi).
CLAWD_LEGS = (4, 6, 11, 13)
CLAWD_VERBS = ("Rimugino", "Elaboro", "Ragiono", "Macino", "Cucino", "Medito", "Combino")


def draw_clawd_body(x, y, u, m, now, k):
    step = (now // 250) % 2
    oled.fill_rect(x + 3 * u, y, 12 * u, 8 * u, 1)  # corpo
    left = right = 2 * u if m == "done" else 0
    if m == "ask":  # saluta con un braccino alla volta
        left = 2 * u if step else 0
        right = 2 * u - left
    oled.fill_rect(x + u, y + 4 * u - left, 2 * u, 2 * u, 1)
    oled.fill_rect(x + 15 * u, y + 4 * u - right, 2 * u, 2 * u, 1)
    for i, c in enumerate(CLAWD_LEGS):
        h = u if m == "work" and i % 2 == step else 2 * u  # passetti
        oled.fill_rect(x + c * u, y + 8 * u, u, h, 1)
    for c in (5, 12):
        ex, ey = x + c * u, y + 2 * u
        if k < 0.5:  # battito di palpebre
            oled.fill_rect(ex, ey + u, u, 1, 0)
        elif m == "work":  # occhi stretti, concentrato
            oled.fill_rect(ex, ey + u, u, u, 0)
        elif m == "done":  # occhi felici
            oled.fill_rect(ex, ey, u, u, 0)
        else:
            oled.fill_rect(ex, ey, u, 2 * u, 0)


def draw_spark(cx, cy, r, now):
    # la stellina che gira mentre Claude pensa
    a0, rr = now / 400, r - (now // 120) % 3
    for i in range(4):
        a = a0 + i * math.pi / 4
        dx, dy = int(rr * math.cos(a)), int(rr * math.sin(a))
        oled.line(cx - dx, cy - dy, cx + dx, cy + dy, 1)
        oled.line(cx - dx + 1, cy - dy, cx + dx + 1, cy + dy, 1)


def draw_clawd(m, now, k):
    if m == "work":
        draw_clawd_body(8, 22 + (now // 250) % 2, 3, m, now, k)
        draw_spark(100, 34, 10, now)
        oled.text(CLAWD_VERBS[(now // 3000) % len(CLAWD_VERBS)] + "." * ((now // 350) % 4), 0, 56)
        if now % 5000 < 1500:  # goccia di sudore per lo sforzo
            sweat_drop(68, 16 + (now % 5000) // 150)
        # consumi di Claude in alto, mentre lavora
        u = state["usage"]
        if u and u[0] != "-":
            oled.text("5h " + u[0] + "%", 0, 0)
        if u and u[1] != "-":
            s = "sett " + u[1] + "%"
            oled.text(s, 128 - 8 * len(s), 0)
    elif m == "ask":
        draw_clawd_body(37, 24 - (3 if (now // 300) % 2 else 0), 3, m, now, k)
        if (now // 400) % 2:
            big_text("?", 100, 20, 2)
    else:
        draw_clawd_body(37, 28 - abs(int(8 * math.sin(now / 150))), 3, m, now, k)
        for i in range(5):  # scintille di festa
            if (now // 200 + i) % 3:
                sx, sy = (6, 100, 16, 112, 24)[i], (20, 24, 44, 50, 58)[i]
                oled.hline(sx - 3, sy, 7, 1)
                oled.vline(sx, sy - 3, 7, 1)


# ---------------------------------------------------------------- motore di disegno
# Le scene usano le stesse coordinate dell'anteprima: area alta 100 con l'origine al centro
# (x da -100 a 100). Qui 1 unità = 0,64 pixel; v_tr, v_sc e v_rot spostano, scalano e ruotano.

PI = math.pi
U = 0.64
TM = [U, 0.0, 0.0, U, 64.0, 32.0]
_tms = []


def v_reset():
    TM[:] = [U, 0.0, 0.0, U, 64.0, 32.0]
    del _tms[:]


def v_save():
    _tms.append(TM[:])


def v_restore():
    TM[:] = _tms.pop()


def v_tr(x, y):
    TM[4] += TM[0] * x + TM[2] * y
    TM[5] += TM[1] * x + TM[3] * y


def v_sc(s):
    for i in range(4):
        TM[i] *= s


def v_rot(r):
    c, s = math.cos(r), math.sin(r)
    a, b, cc, d = TM[0], TM[1], TM[2], TM[3]
    TM[0], TM[1], TM[2], TM[3] = a * c + cc * s, b * c + d * s, cc * c - a * s, d * c - b * s


def mp(x, y):
    return round(TM[0] * x + TM[2] * y + TM[4]), round(TM[1] * x + TM[3] * y + TM[5])


def axis():
    return abs(TM[1]) < 1e-4 and abs(TM[2]) < 1e-4


def scl():
    return math.sqrt(abs(TM[0] * TM[3] - TM[1] * TM[2]))


def P(pts, c=1):
    a = array("h", [0] * len(pts))
    for i in range(0, len(pts), 2):
        a[i], a[i + 1] = mp(pts[i], pts[i + 1])
    oled.poly(0, 0, a, c, True)


def E(x, y, rx, ry, c=1, fill=True, quad=0b1111):
    if rx <= 0 or ry <= 0:
        return
    if axis():
        px, py = mp(x, y)
        oled.ellipse(px, py, round(rx * abs(TM[0])), round(ry * abs(TM[3])), c, fill, quad)
    else:
        P([v for i in range(12) for v in (x + rx * math.cos(i * 0.5236), y + ry * math.sin(i * 0.5236))], c)


def R(x, y, w, h, r=0, c=1):
    if w <= 0 or h <= 0:
        return
    if not axis():
        P([x, y, x + w, y, x + w, y + h, x, y + h], c)
        return
    x0, y0 = mp(x, y)
    x1, y1 = mp(x + w, y + h)
    if x1 < x0:
        x0, x1 = x1, x0
    if y1 < y0:
        y0, y1 = y1, y0
    ww, hh, rr = max(1, x1 - x0), max(1, y1 - y0), round(r * abs(TM[0]))
    if rr >= 2:
        rrect(x0, y0, ww, hh, rr, c)
    else:
        oled.fill_rect(x0, y0, ww, hh, c)


def SR(x, y, w, h, c=1):  # solo il contorno
    x0, y0 = mp(x, y)
    x1, y1 = mp(x + w, y + h)
    oled.rect(min(x0, x1), min(y0, y1), max(1, abs(x1 - x0)), max(1, abs(y1 - y0)), c)


def L(pts, w=1.5, c=1):
    wp = w * scl()
    q = [mp(pts[i], pts[i + 1]) for i in range(0, len(pts), 2)]
    for i in range(1, len(q)):
        (x0, y0), (x1, y1) = q[i - 1], q[i]
        if wp < 1.6:
            oled.line(x0, y0, x1, y1, c)
            continue
        dx, dy = x1 - x0, y1 - y0
        n = math.sqrt(dx * dx + dy * dy) or 1
        ox, oy = -dy / n * wp / 2, dx / n * wp / 2
        oled.poly(0, 0, array("h", [round(x0 + ox), round(y0 + oy), round(x1 + ox), round(y1 + oy),
                                    round(x1 - ox), round(y1 - oy), round(x0 - ox), round(y0 - oy)]), c, True)
        if wp >= 2.5:
            oled.ellipse(x1, y1, round(wp / 2 - 0.5), round(wp / 2 - 0.5), c, True)


def arc_pts(cx, cy, r, a0, a1, ry=None):
    n = max(4, int(abs(a1 - a0) / 6.283 * 20) + 2)
    ry = r if ry is None else ry
    return [v for i in range(n + 1) for v in (cx + r * math.cos(a0 + (a1 - a0) * i / n), cy + ry * math.sin(a0 + (a1 - a0) * i / n))]


def A(x, y, r, a0, a1, w=1.5, c=1):
    L(arc_pts(x, y, r, a0, a1), w, c)


def quad_pts(x0, y0, cx, cy, x1, y1, n=6):
    return [v for i in range(1, n + 1) for v in (
        (1 - i / n) ** 2 * x0 + 2 * (1 - i / n) * (i / n) * cx + (i / n) ** 2 * x1,
        (1 - i / n) ** 2 * y0 + 2 * (1 - i / n) * (i / n) * cy + (i / n) ** 2 * y1)]


def T(s, x, y, c=1):
    px, py = mp(x, y)
    oled.text(s, px - 4 * len(s), py - 4, c)


def rnd(i):
    s = math.sin(i * 12.9898) * 43758.5453
    return s - math.floor(s)


def blinkf(t, off=0):
    p = (t + off) % 4.3
    return abs(p - 0.08) / 0.08 if p < 0.16 else 1


def lookf(t):
    return math.sin(t * 0.7) * 2.5 + math.sin(t * 1.9) * 0.8


# ---------------------------------------------------------------- il personaggio: occhioni, sopracciglia, bocca

EYE_HS = {"excited": 40, "ask": 40, "work": 20, "sleep": 4, "sleepy": 15, "hot": 22, "sad": 30, "stress": 28}
BROWS = {"happy": (5, .18), "excited": (8, .2), "love": (4, .15), "surprised": (10, 0), "sad": (1, .3),
         "stress": (-3, -.3), "sleepy": (-3, .15), "sleep": (-2, .08), "work": (-3, -.38), "ask": (8, 0),
         "cold": (2, .3), "hot": (0, .25), "music": (0, .12)}


def heart(x, y, size, c=1):
    r = size * 0.28
    E(x - r * 0.95, y - r * 0.4, r, r, c)
    E(x + r * 0.95, y - r * 0.4, r, r, c)
    P([x - size * 0.53, y - r * 0.15, x + size * 0.53, y - r * 0.15, x, y + size * 0.55], c)


def brow(x, y, ang, c=1):
    v_save()
    v_tr(x, y)
    v_rot(ang)
    R(-13, -2.6, 26, 5.2, 0, c)
    v_restore()


def mouth(m, t, c=1):
    if m in ("happy", "love", "excited"):
        r, y = (12, 20) if m == "excited" else (10, 21)
        P([-r, y] + arc_pts(0, y, r, 0, PI), c)
        E(0, y + r * 0.65, r * 0.5, r * 0.26, 0)  # lingua
    elif m == "surprised":
        E(0, 27, 6, 7.5, c)
    elif m in ("ask", "talk"):
        E(0, 26, 4.5, 1.5 + abs(math.sin(t * 9)) * 4 if m == "talk" else 5.5, c)
    elif m == "music":
        E(0, 26, 5, 2.5 + abs(math.sin(t * PI * 2)) * 4.5, c)
    elif m == "sleep":
        r = 2.2 + math.sin(t * 1.5) * 0.8
        E(0, 26, r, r, c)
    elif m == "sleepy":
        E(0, 26, 2.5, 3, c)
    elif m == "sad":
        A(0, 35, 9, PI * 1.22, PI * 1.78, 3.2, c)
    elif m == "stress":
        L([-10, 26, -6, 23, -2, 27, 2, 23, 6, 27, 10, 23], 3.2, c)
    elif m == "bored":
        if t % 5 < 1.1:
            E(0, 26, 5, 6.5, c)
        else:
            L([-8, 27, 8, 25], 3.2, c)
    elif m == "work":
        L([-7, 26, 4, 25], 3.2, c)
    elif m == "cold":
        L([-9, 21, 9, 21, 9, 30, -9, 30, -9, 21], 2.4, c)
        j = int(t * 14) % 2
        L([-3 + j, 22, -3 + j, 29], 1.6, c)
        L([3 + j, 22, 3 + j, 29], 1.6, c)
    elif m == "hot":
        E(0, 24, 6, 4.5, c)
    else:
        A(0, 17, 9, PI * 0.22, PI * 0.78, 3.2, c)


def face(m, t, x=0, y=0, s=1, lx=0, ly=0, mm=None, brows=True):
    v_save()
    v_tr(x, y)
    v_sc(s)
    for sd in (-1, 1):
        ex, ey, w, h = sd * 28 + lx, -2 + ly, 26, EYE_HS.get(m, 34)
        if m == "cold":
            ex += 1.2 if int(t * 20) % 2 else -1.2
        if m in ("normal", "cold"):
            h *= blinkf(t)
        raise_, k = BROWS.get(m, (0, 0))
        ang = sd * k
        if m == "bored":
            raise_, ang = (-2, 0) if sd < 0 else (6, -0.15)
        elif m == "music":
            raise_ = abs(math.sin(t * PI * 2)) * 6
        elif m == "normal":
            raise_ = max(0, math.sin(t * 0.9)) * 3
        top, outx, inx = ey - h / 2, ex + sd * (w / 2 + 1), ex - sd * (w / 2 + 1)
        if m == "love":
            heart(ex, ey, 30)
        elif m == "surprised":
            E(ex, ey, 15, 19)
            E(ex, ey, 5.5, 5.5, 0)
        else:
            R(ex - w / 2, top, w, max(2.5, h), 8)
            if m in ("happy", "excited", "music"):
                E(ex, ey + h / 2 + 5, w / 2 + 5, h / 2 + 1, 0)
            elif m == "bored":
                R(ex - w / 2 - 1, top - 1, w + 2, h * 0.5, 0, 0)
            elif m == "ask":
                E(ex, ey, 6.5, 6.5, 0)
            elif m in ("sad", "sleepy", "hot"):
                P([outx, top - 1, inx, top - 1, outx, top + h * 0.6], 0)
            elif m == "stress":
                P([outx, top - 1, inx, top - 1, inx, top + h * 0.5], 0)
        if m == "excited" and int(t * 4) % 2:
            sparkle(ex + sd * 18, ey - 16, 4)
        by = ey - 12 if m == "sleep" else ey - max(h, 38 if m == "surprised" else 20) / 2 - 8 - raise_
        if brows:
            brow(ex, by, ang)
    mouth(mm or m, t)
    v_restore()


# ---------------------------------------------------------------- piccoli oggetti

def zzz(x, y, t):
    for i in range(3):
        p = (t * 0.55 + i / 3) % 1
        T("z", x + p * 12, y - p * 18)


def notes_up(x, y, t, spread=16):
    for i in range(3):
        p = (t * 0.45 + i / 3) % 1
        nx, ny = x + (i - 1) * spread + math.sin(p * 6 + i) * 4, y - p * 26
        E(nx, ny, 2.8, 2.1)
        L([nx + 2.4, ny, nx + 2.4, ny - 8, nx + 5.5, ny - 6], 1.3)


def sweat(x, y):
    E(x, y, 2.3, 2.8)
    P([x - 2.2, y - 1, x, y - 6, x + 2.2, y - 1])


def sparkle(x, y, r):
    L([x - r, y, x + r, y], 1.6)
    L([x, y - r, x, y + r], 1.6)


def star(cx, cy, r, ang):
    for i in range(4):
        a = ang + i * PI / 4
        L([cx - math.cos(a) * r, cy - math.sin(a) * r, cx + math.cos(a) * r, cy + math.sin(a) * r], 2)


def headphones(cx, cy, r):
    A(cx, cy, r, PI * 1.06, PI * 1.94, 3.4)
    R(cx - r - 5, cy - 7, 10, 18, 4)
    R(cx + r - 5, cy - 7, 10, 18, 4)


# ---------------------------------------------------------------- umori

def s_mood(m, t):
    y, lx, ly = 4, 0, 0
    if m == "normal":
        lx, ly = lookf(t) * 2.4, math.sin(t * 0.5) * 1.5
    elif m in ("happy", "excited"):
        y -= abs(math.sin(t * 4)) * 6
    elif m == "stress":
        lx = (int(t * 30) % 3 - 1) * 1.2
    elif m == "bored":
        lx, ly = math.sin(t * 0.5) * 10, 3
    face(m, t, 0, y, 1, lx, ly)
    if m == "stress":
        sweat(58, -22 + (t * 20) % 14)
    elif m == "sleepy" and t % 6 < 1.2:
        T("Uaaah", 70, -34)
    elif m == "sleep":
        zzz(54, -26, t)


# ---------------------------------------------------------------- VS Code

def laptop(t, x=0, y=10):
    R(x - 46, y - 10, 92, 46, 5)
    R(x - 42, y - 6, 84, 38, 3, 0)
    typed, acc = (t * 30) % 220, 0
    for i, (ind, ln) in enumerate(((0, 40), (8, 30), (8, 50), (16, 24), (8, 36), (0, 18))):
        w = max(0, min(ln, typed - acc))
        acc += ln
        R(x - 38 + ind, y - 2 + i * 5.6, w, 3)
    P([x - 56, y + 36, x + 56, y + 36, x + 50, y + 44, x - 50, y + 44])
    R(x - 10, y + 37, 20, 2, 0, 0)


def s_code(t, music):
    if music:
        b = -abs(math.sin(t * PI * 2)) * 2
        face("music", t, 0, -27 + b, 0.55, 0, 4)
        headphones(0, -29 + b, 28)
    else:
        face("normal", t, 0, -27, 0.55, math.sin(t * 1.3) * 5, 4)
    laptop(t)


# ---------------------------------------------------------------- giochi

def g_any(t):  # un giochino a piattaforme sul monitor
    sx, sy, sw, sh, per = -66, -12, 132, 56, 2.2
    ob = sx + sw + 12 - (t % per) / per * (sw + 30)
    hero = sx + 34
    d = ob - hero
    jump = math.cos(d / 18 * PI / 2) * 17 if abs(d) < 18 else 0
    face("excited" if jump > 4 else "stress", t, 0, -33, 0.4, 0, 7, "happy" if jump > 4 else "work")
    for i in range(3):
        A(sx + (i * 60 - t * 6) % 180 - 20, sy + 12, 6, PI, 2 * PI, 1.4)
    for i in range(4):
        A(sx + (i * 50 - t * 14) % 200 - 25, sy + sh - 8, 18, PI, 2 * PI, 1.4)
    gy, x = sy + sh - 9, sx - (t * 34) % 10
    while x < sx + sw:
        R(x, gy, 10, 1.4)
        R(x, gy, 1, 9)
        x += 10
    R(ob - 6, gy - 14, 12, 14)
    R(ob - 8, gy - 17, 16, 5)
    R(ob - 4, gy - 12, 8, 10, 0, 0)
    E(ob + 26, gy - 30, abs(math.cos(t * 6)) * 3.2 + 0.6, 3.8)
    hy = gy - jump
    R(hero - 4, hy - 16, 8, 3)
    R(hero - 3, hy - 13, 6, 4)
    R(hero - 4, hy - 9, 8, 5)
    st = int(t * 10) % 2 if jump < 1 else 0
    R(hero - 4 + st, hy - 4, 3, 4)
    R(hero + 1 - st, hy - 4, 3, 4)
    # cornice del monitor: copre quello che esce dallo schermo
    R(-100, sy - 5, sx + 100, sh + 10, 0, 0)
    R(sx + sw, sy - 5, 100 - sx - sw, sh + 10, 0, 0)
    R(sx - 5, sy - 5, sw + 10, 5)
    R(sx - 5, sy + sh, sw + 10, 5)
    R(sx - 5, sy - 5, 5, sh + 10)
    R(sx + sw, sy - 5, 5, sh + 10)
    px, py = mp(sx + 3, sy + 2)
    oled.text("x%d" % (12 + int(t / per)), px, py)


def g_minecraft(t):
    R(66, -44, 14, 14)
    R(68, -42, 10, 10, 0, 0)
    for i in range(2):
        cx = (i * 110 + t * 6) % 260 - 130
        SR(cx, -40 + i * 10, 30, 5)
        SR(cx + 6, -45 + i * 10, 18, 5)
    gx = -99
    while gx < 99:
        for r in (0, 1):
            SR(gx, 26 + r * 12, 12, 12)
        R(gx, 26, 12, 3)
        gx += 12
    R(-87.5, 10, 3, 16)
    E(-86, 8 + math.sin(t * 20) * 0.6, 3.2, 4.4)
    ph, bx, by = (t % 4) / 4, 30, 2
    if ph <= 0.78:
        SR(bx, by, 24, 24)
        for dx, dy in ((5, 6), (15, 4), (9, 15), (17, 16)):
            R(bx + dx, by + dy, 3, 3)
        for x1, y1, x2, y2 in ((12, 12, 4, 6), (12, 12, 20, 8), (12, 12, 8, 20), (12, 12, 21, 19), (4, 6, 2, 2))[:int(ph / 0.78 * 5)]:
            L([bx + x1, by + y1, bx + x2, by + y2], 1.4)
        swing = math.sin(t * 11) * 0.55 - 0.15
    else:
        p = (ph - 0.78) / 0.22
        for k in range(8):
            a = k * 0.785
            R(bx + 12 + math.cos(a) * p * 18, by + 12 + math.sin(a) * p * 14 + p * p * 10, 3, 3)
        s = abs(math.cos(t * 3)) * 5 + 1
        P([bx + 12, by + 6 - math.sin(t * 4) * 2, bx + 12 + s, by + 12, bx + 12, by + 18, bx + 12 - s, by + 12])
        swing = -0.4
    v_save()
    v_tr(12, -4)
    v_rot(swing)
    L([0, 4, 18, -14], 3)
    P([8, -24, 14, -20, 22, -14, 28, -6, 24, -6, 18, -12, 12, -16, 6, -20])
    v_restore()
    face("work", t, -38, -12, 0.5, 7, 3)


def wave_pts(y0, amp, k, sp, t):
    return [v for x in range(-100, 108, 8) for v in (x, y0 + math.sin(x / k + t * sp) * amp)]


def g_sot(t):
    hz = 10
    A(46, hz + 2, 15, PI, 2 * PI, 1.4)
    L([-99, hz, 99, hz], 1.2)
    for i in range(6):
        w = 16 - i * 1.6
        R(46 - w / 2 + math.sin(t * 3 + i) * 3, hz + 4 + i * 4, w, 1.2)
    for i in range(3):
        p = (t * 0.25 + i / 3) % 1
        x, y = -80 + p * 160, -34 + i * 7 + math.sin(t * 4 + i) * 2
        L([x - 4, y - 2, x, y, x + 4, y - 2], 1.2)
    v_save()
    v_tr(-6, math.sin(t * 1.6) * 2.5 + 2)
    L([0, 18, 0, -46], 2.6)
    L([-26, 18, -26, -30], 2.2)
    L([44, 12, 64, 2], 1.8)
    R(-6, -48, 12, 5)
    bl = math.sin(t * 2) * 1.5
    for x0, y0, x1, y1, bu in ((-23, -26, -2, 8, 4 + bl), (4, -40, 40, 6, 6 + bl)):
        pts = [x0, y0] + quad_pts(x0, y0, (x0 + x1) / 2 + bu, y0 - 2, x1, y0) + [x1, y1] + quad_pts(x1, y1, (x0 + x1) / 2 + bu * 1.4, y1 + 4, x0, y1)
        P(pts, 0)
        L(pts + [x0, y0], 1.4)
    face("happy", t, 24, -18, 0.31)
    fw = math.sin(t * 6) * 2
    P([0, -46, 0, -38, 14, -40 + fw, 14, -46 + fw])
    P([-50, 10, 52, 10] + quad_pts(52, 10, 46, 30, 30, 32) + [-34, 32] + quad_pts(-34, 32, -48, 28, -50, 10))
    for x in range(-36, 40, 14):
        R(x, 16, 5, 4, 0, 0)
    v_restore()
    pts = wave_pts(36, 2.6, 11, 2.4, t)
    P(pts + [100, 50, -100, 50], 0)
    L(pts, 1.2)


CITY = [(-92, 34, 16), (-74, 50, 18), (-54, 28, 14), (-38, 62, 20), (-16, 40, 16),
        (2, 54, 18), (22, 32, 14), (38, 46, 18), (58, 66, 20), (80, 38, 16)]
_win = {}


def windows(key, boxes, step, seed, size):
    """Finestre dei palazzi (posizione in pixel e soglia di accensione), calcolate la prima volta."""
    if key not in _win:
        v_reset()
        out = []
        for i, (x, top, w, bottom, margin) in enumerate(boxes):
            wy = top + margin
            while wy < bottom:
                wx = x + margin - 1
                while wx < x + w - margin:
                    px, py = mp(wx, wy)
                    out.append((px, py, rnd(i * seed + wy * 3 + wx)))
                    wx += step
                wy += step
        _win[key] = (out, max(1, round(size * U)))
    return _win[key]


def g_cities(t):
    for i in range(14):
        if (int(t * 2) + i) % 5:
            px, py = mp(-96 + rnd(i) * 192, -48 + rnd(i + 40) * 34)
            oled.pixel(px, py, 1)
    E(74, -36, 7, 7)
    lit = min(1, (t % 10) / 6) * 0.85
    for i, (x, h, w) in enumerate(CITY):
        SR(x, 38 - h, w, h)
    L([-28, -24, -28, -32], 1)
    win, sz = windows("city", [(x, 38 - h, w, 34, 4) for x, h, w in CITY], 5, 97, 2.4)
    for px, py, r in win:
        if r < lit:
            oled.fill_rect(px, py, sz, sz, 1)
    hy = -38 + math.sin((t % 6) / 6 * PI) * 20
    L([70, 38, 70, -40], 2)
    L([70, -40, 22, -40], 2)
    L([36, -40, 36, hy], 0.8)
    R(31, hy, 10, 6)
    L([-99, 38, 99, 38], 1)
    x = -99
    while x < 99:
        R(x + (t * 20) % 12, 44, 6, 1)
        x += 12
    for i in range(4):
        R((i * 55 + t * 40) % 230 - 115, 40.5, 5, 2)
        R(115 - (i * 60 + t * 34) % 230, 46.5, 5, 2)
    face("happy", t, -54, -32, 0.34, 6, 3)


def g_truck(t):
    hz = -6
    A(-60, hz - 4, 9, PI, 2 * PI, 1.2)
    L([v for x in range(-99, 100, 9) for v in (x, hz - 14 * (0.4 + rnd(x + 3) * 0.8))], 1)
    L([-99, 50, -4, hz], 1)
    L([99, 50, 4, hz], 1)
    for i in range(5):
        k = (1 - (t * 0.8 + i / 5) % 1) ** 2
        y = hz + k * 56
        for s in (-1, 1):
            x = s * (6 + k * 110)
            L([x, y, x, y - 2 - k * 12], 0.8 + k * 2)
    for i in range(3):
        k = (1 - (t * 0.55 + i / 3) % 1) ** 2
        y, x, s = hz + k * 56, -(14 + k * 150), 1 + k * 10
        P([x, y - s * 3, x - s, y, x + s, y])
    Y = -2 + math.sin(t * 9) * 0.5
    for s in (-1, 1):
        L([s * 44, Y - 14, s * 54, Y - 18], 1.6)
        R(s * 54 - 3, Y - 24, 6, 14, 2)
    R(-44, Y - 44, 88, 82, 9)
    R(-40, Y - 49, 80, 7, 3)
    for i in range(5):
        E(-24 + i * 12, Y - 45.5, 2, 1.6, 0)
    R(-37, Y - 38, 74, 36, 5, 0)
    face("happy", t, 0, Y - 18, 0.34)
    wp = math.sin(t * 2.4) * 0.6
    L([-20, Y - 3, -20 + math.cos(-1.2 + wp) * 22, Y - 3 + math.sin(-1.2 + wp) * 22], 1.2)
    R(-34, Y + 2, 68, 18, 3, 0)
    for x in range(-30, 31, 5):
        R(x, Y + 4, 2.4, 14)
    for s in (-1, 1):
        R(s * 39 - 5, Y + 4, 10, 8, 2, 0)
    R(-12, Y + 24, 24, 6, 1, 0)
    for s in (-1, 1):
        R(s * 34 - 8, Y + 32, 16, 10, 3)


def g_isonzo(t):
    L([v for x in range(-99, 100, 12) for v in (x, -6 - 30 * (0.3 + rnd(x * 0.7 + 1)))], 1)
    ex = t % 3.1
    if ex < 1.6:
        p, x = ex / 1.6, -60 + int(t / 3.1) % 3 * 50
        if p < 0.15:
            E(x, -2, 10 * p / 0.15, 7 * p / 0.15)
        for k in range(3):
            E(x + (k - 1) * 6, -4 - p * 18 - k * 3, 5 + p * 6, 4 + p * 4, 1 if p < 0.4 else 0)
    fl = (t * 0.3) % 1
    if fl < 0.9:
        E(50 - fl * 20, -46 + fl * 40, 2, 2)
    for x in range(-96, 99, 32):
        L([x, 6, x, 18], 1.4)
    L([v for x in range(-99, 100, 4) for v in (x, 9 + (2 if (x // 4) % 2 else -1))], 0.8)
    L([v for x in range(-99, 100, 4) for v in (x, 14 + (-1 if (x // 4) % 2 else 2))], 0.8)
    face("stress", t, 0, 14, 0.62, (int(t * 1.5) % 3 - 1) * 6, -2, "sad", False)
    E(0, 4, 36, 24, 1, True, 0b0011)  # elmetto: metà di sopra
    R(-46, 1, 92, 6, 3)
    R(-2, -22, 4, 8, 1)
    for row in (0, 1):
        for i in range(-1, 10):
            x, y = -99 + i * 22 + (11 if row else 0), 44 - row * 9
            E(x, y, 12, 5.5, 0)
            E(x, y, 12, 5.5, 1, False)


RUINS = ((-99, -6, 30), (-70, -30, 26), (-40, -14, 22), (-16, -40, 28), (16, -22, 24), (42, -34, 30), (74, -12, 26))


def g_tlou(t):
    for x, top, w in RUINS:
        L([x, 30, x, top + 6, x + w * 0.3, top, x + w * 0.5, top + 8, x + w * 0.75, top + 2, x + w, top + 10, x + w, 30], 0.9)
    win, sz = windows("ruin", [(x, top + 8, w, 26, 4) for x, top, w in RUINS], 7, 31, 2.6)
    for px, py, r in win:
        if r > 0.75:
            oled.fill_rect(px, py, sz, sz + 1, 1)
    L([-99, 30, 99, 30], 1)
    if math.sin(t * 37) <= 0.93:  # la torcia sfarfalla
        L([-40, 6, 99, -30], 0.8)
        L([-40, 8, 99, 48], 0.8)
    cx, tw = 92 - (t * 10) % 70, math.sin(t * 14) * 0.25
    R(cx - 7, 4, 14, 22, 5)
    L([cx - 6, 8, cx - 14, 18 + math.sin(t * 6) * 3], 3)
    L([cx + 4, 9, cx - 10, 14 + math.cos(t * 6) * 3], 3)
    L([cx - 3, 26, cx - 5, 34], 3)
    L([cx + 3, 26, cx + 4, 34], 3)
    v_save()
    v_tr(cx, -2)
    v_rot(tw)
    E(0, 0, 9, 10)
    for k in range(6):
        a = -PI / 2 + (k - 2.5) * 0.5
        P([math.cos(a) * 5, math.sin(a) * 5, math.cos(a - 0.2) * 15, math.sin(a - 0.2) * 15, math.cos(a + 0.25) * 13, math.sin(a + 0.25) * 13])
    v_restore()
    rx, sw = 110 - (t * 16) % 120, math.sin(t * 12)
    R(rx - 5, 6, 10, 20, 3)
    E(rx, 2, 5.5, 6)
    L([rx - 3, 10, rx - 14, 8 + sw * 2.5], 2.4)
    L([rx + 2, 11, rx - 10, 12 - sw * 2.5], 2.4)
    L([rx - 2, 26, rx - 6 + sw * 3, 34], 2.4)
    L([rx + 2, 26, rx + 5 - sw * 3, 34], 2.4)
    for i in range(16):
        if rnd(i) > 0.3:
            px, py = mp(-96 + (i * 47 + t * 4) % 192, -40 + (i * 29 + math.sin(t + i) * 8) % 80)
            oled.pixel(px, py, 1)
    R(-99, 18, 62, 26, 8)
    R(-94, 10, 44, 12, 5)
    R(-90, 12, 16, 7, 2, 0)
    E(-84, 44, 6, 6, 0)
    E(-50, 44, 6, 6, 0)
    face("surprised", t, -58, -6, 0.4, 6, 0, "stress")
    sweat(-40, -18 + (t * 14) % 8)
    R(-38, -2, 9, 5, 2)


GAMES = {"minecraft": g_minecraft, "sot": g_sot, "cities": g_cities, "ets2": g_truck, "isonzo": g_isonzo, "tlou": g_tlou}
GAME_LED = {"minecraft": (0, 10, 0), "sot": (0, 6, 8), "cities": (10, 8, 0), "ets2": (12, 4, 0),
            "isonzo": (8, 6, 0), "tlou": (2, 8, 2)}


# ---------------------------------------------------------------- musica e video

def s_music(t):
    sway, b = math.sin(t * PI) * 12, -math.sin(PI * ((t * 2) % 1)) * 5
    face("music", t, sway, b - 4, 0.66)
    headphones(sway, b - 7, 30)
    notes_up(-74, 2, t, 10)
    notes_up(76, -2, t + 0.5, 10)


def popcorn(t, x, y):
    P([x - 16, y - 12, x + 16, y - 12, x + 12, y + 20, x - 12, y + 20])
    for i in (-1, 0, 1):
        L([x + i * 10, y - 10, x + i * 8, y + 18], 2, 0)
    for i in range(6):
        E(x - 12 + i * 5, y - 14 - (i % 2) * 3, 4, 3.4)
    p = (t * 1.3) % 1
    E(x + 6, y - 18 - math.sin(p * PI) * 14, 2.8, 2.4)


def s_video(t):
    face("normal", t, -22, -10, 0.7, -4, 0, "surprised" if int(t / 3) % 3 == 2 else None)
    popcorn(t, 62, 0)


def fmt_time(s):
    return "%d:%02d" % (s // 60, s % 60)


def draw_media_footer(now):
    """In basso: titolo che scorre e, sopra, una riga sottile che avanza col brano."""
    oled.fill_rect(0, 53, 128, 11, 0)
    b = state["media_pos"]
    if b and b[1] > 0:
        pos, dur, at = b
        cur = min(dur, pos + max(0, time.ticks_diff(now, at)) // 1000)  # avanza anche tra un aggiornamento e l'altro
        oled.hline(0, 54, 128, 1)
        oled.fill_rect(0, 53, 128 * cur // dur, 3, 1)
    song = state["song"]
    if song:
        oled.text(song, 128 - (now // 35) % (len(song) * 8 + 128), 57)


# ---------------------------------------------------------------- OpenStreetMap

def map_bg():
    L([-99, -8, 99, -2], 5)
    L([-20, -50, -14, 50], 5)
    L([-99, 34, 99, 26], 4)
    L([24, -50, 30, 50], 3)
    for x, y, w, h in ((-30, -40, 22, 16), (0, -42, 18, 22), (-82, 2, 26, 18), (-50, 20, 24, 20), (8, -10, 14, 12)):
        SR(x, y, w, h)


def s_osm_view(t):
    map_bg()
    lx, ly = math.sin(t * 0.6) * 40, math.sin(t * 0.9) * 10
    E(lx, ly, 34, 34, 0)
    A(lx, ly, 34, 0, 2 * PI, 4)
    L([lx + 24, ly + 24, lx + 40, ly + 40], 6)
    face("happy", t, lx, ly + 1, 0.48)


def s_osm_edit(t):
    map_bg()
    pts = ((-10, -6), (40, -10), (44, 26), (-6, 30))
    n = int(t * 1.2) % 6
    for i in range(min(n, 4)):
        if i < n - 1 or n >= 4:
            L([pts[i][0], pts[i][1], pts[(i + 1) % 4][0], pts[(i + 1) % 4][1]], 2.2)
    for i in range(min(n + 1, 4)):
        E(pts[i][0], pts[i][1], 3.4, 3.4)
    c = pts[min(n, 3)]
    P([c[0] + 2, c[1] + 2, c[0] + 2, c[1] + 14, c[0] + 6, c[1] + 10, c[0] + 11, c[1] + 10])
    R(-99, -50, 64, 44, 6, 0)
    face("work", t, -67, -30, 0.4, 6, 3)


# ---------------------------------------------------------------- frasi (nuvoletta)

def wrap(text, width=15, rows=2):
    lines, cur = [], ""
    for word in text.split():
        if cur and len(cur) + 1 + len(word) > width:
            lines.append(cur)
            cur = word
        else:
            cur = (cur + " " + word).strip()
    lines.append(cur)
    return [l[:width] for l in lines[:rows]]


SPEECH_MOOD = ("happy", "excited", "love", "surprised", "sad", "sleepy", "bored")


def s_speech(emo, text, t):
    face(emo if emo in SPEECH_MOOD else "normal", t, -72, 8, 0.42, 0, 0, "talk")
    R(-46, -48, 144, 72, 10)
    P([-46, -2, -58, 10, -34, 6])
    lines = wrap(text, 10, 4)
    for i, l in enumerate(lines):
        px, py = mp(26, -12)
        oled.text(l, px - 4 * len(l), py - 4 + round((i - (len(lines) - 1) / 2) * 10), 0)


# ---------------------------------------------------------------- meteo (animazione ogni tanto)
# Non è una pagina: ogni 4-7 minuti (anche mentre programmi, sei via o di notte), per 8 secondi
# reagisce al tempo che fa: trema, suda, apre l'ombrello, mette gli occhiali da sole...

WEATHER_ANIM_MS = 8000
WEATHER_EVERY = (4 * 60000, 7 * 60000)
WEATHER_LED = {"cold": (0, 4, 12), "hot": (14, 4, 0), "rain": (0, 2, 10), "snow": (8, 8, 10),
               "storm": (4, 0, 8), "sun": (14, 10, 0), "moon": (2, 2, 6), "fog": (3, 3, 3),
               "cloud": (4, 5, 6)}
wx = {"kind": None, "until": 0, "next": None, "forced": False, "inverted": False, "turn": False}


def sweat_drop(x, y):  # in pixel (la usa la mascotte di Claude)
    oled.ellipse(x, y, 2, 2, 1, True)
    oled.poly(0, 0, array("h", [x - 2, y - 1, x + 2, y - 1, x, y - 6]), 1, True)


def sunglasses(ex, ey):
    for s in (-1, 1):
        R(ex + s * 28 - 18, ey - 13, 36, 26, 9)
        R(ex + s * 28 - 15, ey - 10, 30, 20, 7, 0)
        L([ex + s * 28 - 9, ey + 5, ex + s * 28 - 1, ey - 5], 1.4)
    R(ex - 10, ey - 9, 20, 3)
    L([ex - 46, ey - 8, -99, ey - 10], 2)
    L([ex + 46, ey - 8, 99, ey - 10], 2)


def sun_corner(t, x=80, y=-34):
    E(x, y, 8, 8)
    for i in range(8):
        a = i * 0.785 + t * 0.6
        L([x + math.cos(a) * 11, y + math.sin(a) * 11, x + math.cos(a) * 15, y + math.sin(a) * 15], 2)


def umbrella(t):
    E(0, -16, 70, 30, 1, True, 0b0011)
    for x in range(-60, 61, 20):
        E(x, -16, 10, 4, 0, True, 0b0011)
    L([0, -46, 0, -50], 2)
    for i in range(16):
        x = -96 + (i * 37) % 192
        y = -50 + (t * 60 + i * 9) % 6 if abs(x) < 72 else -50 + (t * 90 + i * 17) % 100
        if abs(x) >= 72:
            L([x, y, x - 1, y + 5], 1.4)
    L([0, -16, 0, 44], 2.4)


def flakes(t, n):
    for i in range(n):
        sparkle(-96 + (i * 37 + math.sin(t * 0.8 + i) * 6) % 192, -50 + (t * 16 + i * 29) % 100, 1.8)


def storm_face(t, k):  # da tranquillo (k = 0) a spaventato (k = 1)
    v_save()
    v_tr((1.5 if int(t * 30) % 2 else -1.5) * k if k > 0.6 else 0, 14)
    v_sc(0.8)
    for sd in (-1, 1):
        ex, ey, w = sd * 28, -2 - 2 * (1 - k), 26 + 4 * k
        h = (34 + 4 * k) * (blinkf(t) if k < 0.05 else 1)
        R(ex - w / 2, ey - h / 2, w, max(2.5, h), 8 + (w / 2 - 8) * k)
        if k > 0.15:
            E(ex, ey, 5.5 * k, 5.5 * k, 0)
        brow(ex, ey - h / 2 - 8 - 10 * k, sd * 0.12 * (1 - k))
    if k < 0.25:
        mouth("normal", t)
    else:
        E(0, 26 + k, 2 + 4 * k, 2 + 5.5 * k)
    v_restore()


def s_weather(kind, now, t):
    if kind == "sun":
        face("happy", t, 0, 4)
        sunglasses(0, 2)
        sun_corner(t)
    elif kind == "rain":
        umbrella(t)
        face("normal", t, 0, 14, 0.8, 0, 2)
    elif kind == "storm":
        ph = (now % 1800) / 1000
        fear = ph / 0.08 if ph < 0.08 else 1 if ph < 0.5 else max(0, 1 - (ph - 0.5) / 0.7)
        umbrella(t)
        storm_face(t, fear)
        if ph < 0.25:
            P([-86, -50, -74, -50, -80, -32, -70, -32, -88, -4, -82, -26, -90, -26])
    elif kind == "snow":
        flakes(t, 18)
        face("happy", t, 0, 4)
    elif kind == "cold":
        face("cold", t, 0, -4)
        R(-50, 36, 100, 10, 5)
        for x in range(-44, 50, 14):
            R(x, 37, 6, 8, 0, 0)
        if int(t * 2.5) % 2:
            T("Brrr!", 76, -38)
    elif kind == "hot":
        face("hot", t, 0, 2)
        sun_corner(t)
        sweat(-60, -14 + (t * 18) % 26)
        sweat(62, -6 + (t * 18 + 13) % 26)
        for x in range(-96, 99, 6):
            R(x, 46 + (1 if (x // 6 + int(t * 6)) % 4 < 2 else 0), 4, 1.6)
    elif kind == "moon":
        E(78, -32, 10, 10)
        E(83, -36, 9, 9, 0)
        for i, (x, y) in enumerate(((-86, -36), (-50, -20), (-12, -42), (40, -30), (-70, 40), (66, 38), (24, 42))):
            ph = int(t * 3 + i) % 3
            if ph:
                sparkle(x, y, 2.5 if ph == 1 else 1)
        face("sleepy", t, 0, 2)
    elif kind == "fog":
        face("normal", t, 0, 2, 1, math.sin(t) * 4)
        for r in range(5):
            y, off = -40 + r * 20, (t * 12 % 40) * (1 if r % 2 else -1)
            for x in range(-140, 99, 40):
                R(x + off, y, 28, 5, 2, (r + x // 40) % 2)
    else:  # nuvoloso
        x = 110 - (t * 22) % 240
        face("normal", t, 0, 8, 1, max(-6, min(6, x / 12)), -4)
        for dx, dy, r in ((0, -36, 10), (12, -42, 12), (24, -36, 10), (12, -34, 10)):
            E(x + dx, dy, r, r)


# ---------------------------------------------------------------- feste (stesso giro del meteo)
# Il PC manda "F <festa>" nei giorni di festa; l'animazione si alterna con quella del meteo.

FEAST_LED = {"halloween": (14, 5, 0), "natale": (12, 0, 0), "capodanno": (10, 8, 0), "befana": (6, 0, 8),
             "sanvalentino": (14, 0, 4), "carnevale": (8, 0, 10), "pasqua": (8, 8, 2), "italia": (0, 10, 0),
             "ferragosto": (14, 9, 0)}


def pumpkin(x, y):
    E(x, y, 13, 10)
    L([x, y - 10, x + 1, y - 14], 3)
    P([x - 8, y - 1, x - 3, y - 1, x - 5.5, y - 6], 0)
    P([x + 3, y - 1, x + 8, y - 1, x + 5.5, y - 6], 0)
    R(x - 6, y + 3, 12, 3, 0, 0)


def bat(x, y, t):
    up = int(t * 7) % 2
    E(x, y, 3, 3)
    P([x - 2, y, x - 13, y - 6 if up else y + 4, x - 6, y + 2])
    P([x + 2, y, x + 13, y - 6 if up else y + 4, x + 6, y + 2])


def burst(x, y, p):
    r1, r2 = 3 + 16 * p, 8 + 22 * p
    for i in range(12):
        a = i * 0.5236
        L([x + math.cos(a) * r1, y + math.sin(a) * r1, x + math.cos(a) * r2, y + math.sin(a) * r2], 1.6)


def egg(x, y):
    E(x, y, 8, 10.5)
    L([x - 7, y, x - 4, y - 3, x - 1, y, x + 2, y - 3, x + 5, y], 1.4, 0)
    R(x - 7, y + 4, 15, 2, 0, 0)


def flag(x, y, t):  # tricolore che sventola: verde a puntini, bianco vuoto, rosso pieno
    L([x - 1, y, x - 1, y + 34], 1.5)
    for col in range(14):
        dy = 2.4 * math.sin(t * 6 + col / 2)
        cx = x + col * 2
        if col < 5:
            for yy in range(0, 20, 4):
                R(cx, y + dy + yy + (col % 2) * 2, 1.6, 1.6)
        elif col >= 9:
            R(cx, y + dy, 2, 20)
        else:
            R(cx, y + dy, 2, 1.6)
            R(cx, y + dy + 18.4, 2, 1.6)


def s_feast(kind, t):
    if kind == "halloween":
        E(78, -34, 11, 11)
        for i in range(3):
            bat(-110 + (t * 30 + i * 80) % 230, -38 + i * 8 + math.sin(t * 3 + i) * 4, t + i * 0.1)
        face("stress", t, 0, 10, 0.82, 0, 0, "excited")
        P([-28, -14, 28, -14, 18, -50])
        R(-54, -17, 108, 7, 3)
        R(-24, -22, 48, 4, 0, 0)
        pumpkin(-80, 38)
        pumpkin(80, 38)
    elif kind == "natale":
        flakes(t, 14)
        face("happy", t, 0, 10, 0.85)
        P([-48, -18, 40, -18, 64, -44])
        E(66, -44, 6, 6)
        R(-54, -24, 100, 9, 4)
        L([-54, -25, 46, -25], 1.2, 0)
        P([-80, -34, -94, -6, -66, -6])
        P([-80, -22, -98, 10, -62, 10])
        R(-83, 10, 6, 7)
    elif kind == "capodanno":
        for i, (x, y) in enumerate(((-74, -26), (74, -22), (-20, -40), (40, 34), (-70, 30))):
            p = (t * 0.6 + i / 5) % 1
            if p < 0.8:
                burst(x, y, p / 0.8)
        face("excited", t, 0, 10, 0.8)
        P([-12, -16, 12, -16, 0, -46])
        E(0, -46, 3.5, 3.5)
    elif kind == "befana":
        x, y = 110 - (t * 26) % 240, -32 + math.sin(t * 5) * 3
        L([x - 22, y + 8, x + 18, y + 3], 2)
        for d in range(5):
            L([x + 18, y + 3, x + 28, y - 3 + d * 3], 1.2)
        R(x - 7, y - 7, 10, 12, 3)
        E(x - 2, y - 9, 3.5, 3.5)
        P([x - 9, y - 11, x + 5, y - 11, x - 3, y - 24])
        face("surprised", t, 0, 12, 0.75, max(-6, min(6, x / 14)), -3)
    elif kind == "sanvalentino":
        for i in range(8):
            x = -86 + (i * 7) % 20 if i % 2 else 86 - (i * 7) % 20
            heart(x, 56 - (t * 24 + i * 29) % 120, 8 + (i % 3) * 3)
        face("love", t, 0, 4)
    elif kind == "carnevale":
        for i in range(20):
            R(-96 + (i * 41 + math.sin(t * 2 + i) * 5) % 192, -50 + (t * 26 + i * 13) % 100, 2.6, 2.6)
        for s in (-1, 1):
            R(s * 28 - 22, -28, 44, 44, 14)
            R(s * 28 - 19, -25, 38, 38, 12, 0)
        R(-8, -18, 16, 8)
        face("happy", t, 0, 4, 0.9)
    elif kind == "pasqua":
        for s in (-1, 1):
            ex = s * 20 + (math.sin(t * 4) * 2 if s > 0 else 0)
            E(ex, -38, 8, 16)
            E(ex, -36, 4, 12, 0)
        face("happy", t, 0, 10, 0.82)
        egg(-80, 38)
        egg(80, 38)
    elif kind == "italia":
        flag(-96, -26, t)
        flag(70, -26, t + 0.4)
        face("happy", t, 0, 4, 0.9)
    else:  # ferragosto
        face("happy", t, 0, 4)
        sunglasses(0, 2)
        sun_corner(t)
        by = 38 - abs(math.sin(t * 4.5)) * 18
        E(-78, by, 10, 10)
        L([-78, by - 10, -78, by + 10], 1.4, 0)
        A(-78, by, 6, -1.2, 1.2, 1.2, 0)


def weather_led(kind, now):
    if kind == "storm" and now % 1800 < 120:
        return (16, 16, 16)
    if kind == "capodanno":  # fuochi colorati
        return wheel(now // 8 % 256)
    return FEAST_LED.get(kind) or WEATHER_LED.get(kind, COLORS["normal"])


def wheel(pos):
    """Colore dell'arcobaleno (0-255), a bassa luminosità."""
    if pos < 85:
        return (12 - pos * 12 // 85, pos * 12 // 85, 0)
    if pos < 170:
        pos -= 85
        return (0, 12 - pos * 12 // 85, pos * 12 // 85)
    pos -= 170
    return (pos * 12 // 85, 0, 12 - pos * 12 // 85)


def top_bar(m, now):
    if m == "ask":
        if (now // 400) % 2:
            center_text("Ti cerco!", 0)
    elif m == "done":
        center_text("Fatto!", 0)
    elif m in ("normal", "happy", "stress", "bored", "sleepy"):
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
    if board:
        board.attach(globals())
    splash()
    now = time.ticks_ms()
    blink_at, blink_i = time.ticks_add(now, 2000), -1
    look_at, lx, ly, tx, ty = now, 0.0, 0.0, 0, 0
    last_mood, last_led = None, None
    page, page_until, screen_off = 0, now, False

    while True:
        read_serial()
        now = time.ticks_ms()
        if board:
            board.tick(now)
        m = mood()
        if m != last_mood:
            look_at = now  # nuovo umore: riposiziona subito lo sguardo
            last_mood = m
        msg = state["msg"]
        if msg and time.ticks_diff(now, msg[2]) > 0:
            state["msg"] = msg = None
        speaking = msg is not None and m not in ("ask", "sleep")

        # tasto BOOT e pagine
        ev = board.button(now) if board else read_button(now)
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
        idle_mood = m in ("normal", "happy", "stress", "code", "bored", "sleepy") and not speaking and not page
        if state["wtest"]:
            wx["kind"], wx["until"], wx["forced"] = state["wtest"], time.ticks_add(now, WEATHER_ANIM_MS), True
            state["wtest"] = None
        elif idle_mood and (state["weather"] or state["feast"]) and wx["kind"] is None:
            if wx["next"] is None:
                wx["next"] = time.ticks_add(now, 15000)  # la prima volta poco dopo l'avvio
            elif time.ticks_diff(now, wx["next"]) > 0:
                # nei giorni di festa si alterna con il meteo
                wx["turn"] = not wx["turn"]
                kind = state["feast"] if state["feast"] and (wx["turn"] or not state["weather"]) else state["weather"]
                wx["kind"], wx["until"], wx["forced"] = kind, time.ticks_add(now, WEATHER_ANIM_MS), False
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
        if board:
            board.leds(now, m, screen_off)

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
        if board and board.draw(now):  # volume, microfono, tasto appena premuto
            oled.show()
            time.sleep_ms(30)
            continue
        if page:
            PAGES[page](now)
            oled.show()
            time.sleep_ms(30)
            continue
        t = (now % 3600000) / 1000  # secondi, ricominciano ogni ora (i float qui sono a 32 bit)
        v_reset()
        if speaking:
            s_speech(msg[0], msg[1], t)
        elif weather_on:
            if wx["kind"] in FEAST_LED:
                s_feast(wx["kind"], t)
            else:
                s_weather(wx["kind"], now, t)
        elif m in ACTIVITIES:  # Claude Code: la mascotte
            draw_clawd(m, now, k)
        elif m == "code":
            s_code(t, state["music"])
        elif m == "game":
            GAMES.get(state["detail"], g_any)(t)
        elif m == "osm":
            (s_osm_edit if state["detail"] == "edit" else s_osm_view)(t)
        elif m in ("music", "video"):
            (s_music if m == "music" else s_video)(t)
            draw_media_footer(now)
        else:
            s_mood(m, t)
        if not speaking and not weather_on:
            top_bar(m, now)
        if board:
            board.decorate(now)
        oled.show()
        time.sleep_ms(30)


try:
    main()
except Exception as e:  # non restare fermo sulla console: salva l'errore in errore.txt e riparti
    with open("errore.txt", "a") as f:
        f.write("--- %d ms dall'avvio\n" % time.ticks_ms())
        sys.print_exception(e, f)
    import machine
    machine.reset()
