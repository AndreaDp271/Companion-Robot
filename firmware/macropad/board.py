# Desktop Companion - Adafruit MacroPad RP2040: display, LED, tasti ed encoder.
# Il firmware è lo stesso del robot (firmware/rp2040/main.py, copiato come main.py): se trova
# questo modulo usa il display e i 12 LED del macropad e gli passa tasti ed encoder.
# Al PC manda righe che iniziano con "!":
#   "!hello macropad <versione>"  ogni 5 s, così l'app sa che questo è il macropad
#   "!k <n>" / "!h <n>"           tasto n (0-11, "enc" = encoder) premuto / tenuto premuto
#   "!e spk|mic <scatti>"         encoder girato (mic se lo giri tenendolo premuto)
# Dal PC, oltre alle righe del robot:
#   "K <vol%> <muto> <mic%> <muto>"  audio letto da AudioTray ("-" se non risponde)
#   "D <nome>"                       uscita attiva (Casse, Cuffie...), mostrata in basso con il microfono
#   "Y <n> <rrggbb> <ruolo> <nome>"  tasto n: colore fisso, ruolo (mic spk media app companion) e nome
#   "O <nome>"                       uscita audio appena scelta
# Lo speaker del macropad resta sempre spento.
import time, math, framebuf, neopixel, machine
from array import array
from machine import Pin, SPI

# speaker: amplificatore spento e uscita audio a massa, prima di tutto il resto
Pin(14, Pin.OUT, value=0)
Pin(16, Pin.OUT, value=0)

VERSION = "1.0.0"
HOLD_MS = 600  # pressione lunga
OVERLAY_MS = 1600  # quanto resta a schermo volume / nome del tasto
STEP = 2  # % di volume per scatto dell'encoder (lo stesso usato dal PC)
ENC_DIR = -1  # verso dell'encoder: orario = volume su
ENC = 12  # indice della pressione dell'encoder tra i tasti


class SH1106(framebuf.FrameBuffer):
    """Display SH1106 128x64 del macropad, su SPI1: stessi metodi dell'SSD1306 usati dal robot."""

    def __init__(self):
        self.spi = SPI(1, 8_000_000, sck=Pin(26), mosi=Pin(27), miso=Pin(28))
        self.dc, self.cs = Pin(24, Pin.OUT, value=0), Pin(22, Pin.OUT, value=1)
        rst = Pin(23, Pin.OUT, value=1)
        self.buf = bytearray(1024)
        self.pages = [memoryview(self.buf)[p * 128:(p + 1) * 128] for p in range(8)]
        # pagina p, colonna 2: la RAM dell'SH1106 è larga 132, il vetro inizia dalla colonna 2
        self.addr = [bytes((0xB0 | p, 0x02, 0x10)) for p in range(8)]
        super().__init__(self.buf, 128, 64, framebuf.MONO_VLSB)
        rst(0)
        time.sleep_ms(10)
        rst(1)
        time.sleep_ms(10)
        self.cmd(bytes((0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xAD, 0x8B, 0xA1, 0xC8,
                        0xDA, 0x12, 0x81, 0xFF, 0xD9, 0x1F, 0xDB, 0x40, 0x33, 0xA6, 0xA4)))
        self.fill(0)
        self.show()
        self.poweron()

    def cmd(self, data):
        self.dc(0)
        self.cs(0)
        self.spi.write(data)
        self.cs(1)

    def show(self):
        for p in range(8):
            self.cmd(self.addr[p])
            self.dc(1)
            self.cs(0)
            self.spi.write(self.pages[p])
            self.cs(1)

    def poweron(self):
        self.cmd(b"\xaf")

    def poweroff(self):
        self.cmd(b"\xae")

    def invert(self, v):
        self.cmd(b"\xa7" if v else b"\xa6")


class NoLed:
    """Il robot ha un LED che cambia colore con l'umore: qui i tasti hanno luci fisse, lo ignoro."""

    def __setitem__(self, i, c):
        pass

    def write(self):
        pass


oled = SH1106()
led = NoLed()
pix = neopixel.NeoPixel(Pin(19), 12)

# ---------------------------------------------------------------- tasti ed encoder

PINS = [Pin(i, Pin.IN, Pin.PULL_UP) for i in range(1, 13)] + [Pin(0, Pin.IN, Pin.PULL_UP)]
_latched = 0  # tasti premuti dall'ultimo controllo (anche se già rilasciati)


def _latch(bit):
    global _latched
    _latched |= bit


for _i, _p in enumerate(PINS):
    _p.irq(lambda p, b=1 << _i: _latch(b), Pin.IRQ_FALLING)

_ra, _rb = Pin(17, Pin.IN, Pin.PULL_UP), Pin(18, Pin.IN, Pin.PULL_UP)
QUAD = (0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0)
_es = (_ra.value() << 1) | _rb.value()
_sub = 0  # quarti di scatto accumulati


def _enc_irq(p):
    global _es, _sub
    s = (_ra.value() << 1) | _rb.value()
    _sub += QUAD[(_es << 2) | s]
    _es = s


_ra.irq(_enc_irq, Pin.IRQ_FALLING | Pin.IRQ_RISING, hard=True)
_rb.irq(_enc_irq, Pin.IRQ_FALLING | Pin.IRQ_RISING, hard=True)

# ruolo, colore, nome: quelli veri arrivano dal PC (righe Y), questi valgono finché non c'è
keys = [["spk", (255, 255, 255), "Audio"], ["mic", (0, 255, 0), "Microfono"],
        ["app", (255, 208, 0), "CastelSafe"], ["media", (255, 0, 176), "Indietro"],
        ["media", (255, 0, 176), "Play/Pausa"], ["media", (255, 0, 176), "Avanti"],
        ["app", (0, 64, 255), "VS Code"], ["app", (255, 48, 0), "Brave"],
        ["app", (0, 192, 160), "Esplora file"], ["app", (112, 32, 255), "Discord"],
        ["app", (0, 160, 255), "Telegram"], ["companion", (128, 255, 0), "Companion"]]
down = [None] * 13  # quando è stato premuto, None se è su
held = [False] * 13  # pressione lunga già segnalata
released = [0] * 13
enc_turned = False  # girato mentre era premuto: non conta come clic
events = []  # per il robot: "short"/"long" dal tasto Companion, "any" per qualsiasi tasto
audio = {"spk": None, "spkm": False, "mic": None, "micm": False, "hold": 0, "out": ""}
cur_mood = "sleep"  # umore del robot, per sapere se c'è posto per la barra in basso
ov = {"kind": None, "until": 0, "label": "", "sub": ""}
hello_at = 0
g = {}  # funzioni e stato del firmware del robot (vedi attach)


def attach(main_globals):
    g.update(main_globals)


def send(s):
    print(s)


def overlay(kind, label="", sub=""):
    ov["kind"], ov["label"], ov["sub"] = kind, label, sub
    ov["until"] = time.ticks_add(time.ticks_ms(), OVERLAY_MS)


def parse(line):
    """Righe dal PC che il robot non conosce. True se l'ha gestita."""
    p = line.split(" ", 4)
    if p[0] == "K" and len(p) == 5:
        if time.ticks_diff(time.ticks_ms(), audio["hold"]) < 0:
            return True  # hai appena girato o premuto: il PC non ha ancora il valore nuovo
        audio["spk"] = int(p[1]) if p[1].isdigit() else None
        audio["mic"] = int(p[3]) if p[3].isdigit() else None
        audio["spkm"], audio["micm"] = p[2] == "1", p[4] == "1"
        return True
    if p[0] == "Y" and len(p) == 5 and p[1].isdigit() and int(p[1]) < 12:
        c = int(p[2], 16)
        keys[int(p[1])] = [p[3], (c >> 16, (c >> 8) & 255, c & 255), p[4]]
        return True
    if p[0] == "O":
        overlay("key", "Uscita audio", line[2:])
        return True
    if p[0] == "D":
        audio["out"] = line[2:]
        return True
    return False


def _toggle(kind):
    if audio[kind] is not None:
        audio[kind + "m"] = not audio[kind + "m"]
        audio["hold"] = time.ticks_add(time.ticks_ms(), 400)
    overlay(kind)


def _tap(i, now):
    if i == ENC:
        if not enc_turned:
            if time.ticks_diff(now, down[i]) > HOLD_MS:
                send("!h enc")
                overlay("key", "AudioTray", "pannello")
            else:
                send("!k enc")  # casse <-> cuffie: il nome nuovo arriva dal PC (riga O)
                overlay("key", "Uscita audio", "cambio...")
        return
    role, _, label = keys[i]
    if role == "companion":
        events.append("short")
        return
    send("!k %d" % i)
    if role in ("mic", "spk"):
        _toggle(role)
    elif role == "media" and g.get("state", {}).get("song"):
        overlay("key", label, g["state"]["song"])
    else:
        overlay("key", label)


def _hold(i):
    role, _, label = keys[i]
    if role == "companion":
        events.append("long")
        return
    send("!h %d" % i)
    overlay("key", label, "tenuto premuto")


def tick(now):
    """Legge tasti ed encoder: da chiamare a ogni giro del ciclo principale."""
    global _latched, _sub, enc_turned, hello_at
    st = machine.disable_irq()
    lat, _latched = _latched, 0
    q = _sub
    steps = int(q / 4)
    _sub = q - steps * 4
    machine.enable_irq(st)

    for i, p in enumerate(PINS):
        now_down = not p.value()
        if down[i] is None:
            # un fronte appena dopo il rilascio è il rimbalzo del contatto, non una pressione
            if now_down or (lat >> i & 1 and time.ticks_diff(now, released[i]) > 80):
                down[i], held[i] = now, False
                if i == ENC:
                    enc_turned = False
                events.append("any")
            else:
                continue
        if now_down:
            if i != ENC and not held[i] and time.ticks_diff(now, down[i]) > HOLD_MS:
                held[i] = True
                _hold(i)
        else:
            if not held[i]:
                _tap(i, now)
            down[i], released[i] = None, now

    if steps:
        steps *= ENC_DIR
        if abs(steps) > 1:  # girato in fretta: passi più lunghi
            steps *= 2
        kind = "mic" if down[ENC] is not None else "spk"
        if kind == "mic":
            enc_turned = True
        send("!e %s %d" % (kind, steps))
        if audio[kind] is not None:
            audio[kind] = max(0, min(100, audio[kind] + steps * STEP))
            if steps > 0:
                audio[kind + "m"] = False
            audio["hold"] = time.ticks_add(now, 400)
        overlay(kind)
        events.append("any")

    if time.ticks_diff(now, hello_at) >= 0:
        send("!hello macropad " + VERSION)
        hello_at = time.ticks_add(now, 5000)


def button(now):
    """Come read_button del robot: "short", "long", "any" o None."""
    return events.pop(0) if events else None


# ---------------------------------------------------------------- LED dei tasti

def _dim(c, k=36):
    return (c[0] * k // 255, c[1] * k // 255, c[2] * k // 255)


_last_pix = None


def key_color(i, off, online):
    """Ogni tasto ha la sua luce fissa; audio e microfono diventano rossi quando sono muti."""
    role, col, _ = keys[i]
    if online and ((role == "mic" and audio["mic"] is not None and audio["micm"])
                   or (role == "spk" and audio["spk"] is not None and audio["spkm"])):
        return (36, 0, 0)
    if off or not online:  # schermo spento o PC spento: luci spente
        return (0, 0, 0)
    return _dim(col)


def leds(now, m, off):
    global _last_pix, cur_mood
    cur_mood = m
    st = g["state"]
    online = st["last"] is not None and time.ticks_diff(now, st["last"]) < 10000
    cols = tuple(key_color(i, off, online) for i in range(12))
    if cols != _last_pix:
        for i, c in enumerate(cols):
            pix[i] = c
        pix.write()
        _last_pix = cols


# ---------------------------------------------------------------- schermo

def icon_speaker(x, y, vol, muted):
    oled.fill_rect(x + 2, y + 11, 7, 10, 1)
    oled.poly(0, 0, array("h", [x + 8, y + 11, x + 16, y + 3, x + 16, y + 28, x + 8, y + 20]), 1, True)
    if muted:
        for d in (0, 1):
            oled.line(x + 20 + d, y + 10, x + 30 + d, y + 22, 1)
            oled.line(x + 20 + d, y + 22, x + 30 + d, y + 10, 1)
        return
    for r, lim in ((5, 0), (9, 33), (13, 66)):
        if vol is None or vol > lim:
            oled.ellipse(x + 17, y + 16, r, r, 1, False, 0b1001)


def icon_mic(x, y, muted):
    g["rrect"](x + 11, y + 1, 10, 18, 5)
    oled.ellipse(x + 16, y + 13, 10, 10, 1, False, 0b1100)
    oled.vline(x + 16, y + 23, 5, 1)
    oled.hline(x + 10, y + 28, 13, 1)
    if muted:
        for d in (-2, -1, 2):  # bordo nero, poi la barra
            oled.line(x + 4 + d, y + 1, x + 28 + d, y + 30, 0)
        for d in (0, 1):
            oled.line(x + 4 + d, y + 1, x + 28 + d, y + 30, 1)


def draw_level(kind):
    v, muted = audio[kind], audio[kind + "m"]
    g["center_text"]("VOLUME" if kind == "spk" else "MICROFONO", 0)
    if kind == "spk":
        icon_speaker(0, 16, v, muted)
    else:
        icon_mic(0, 16, muted)
    if v is None:
        g["center_text"]("AudioTray", 26)
        g["center_text"]("non risponde", 38)
        return
    txt = "OFF" if muted else "%d%%" % v
    g["big_text"](txt, max(32, 128 - 24 * len(txt)), 18, 3)
    oled.rect(0, 52, 128, 12, 1)
    w = 124 * v // 100
    if muted:  # barra tratteggiata: il livello resta, ma è spento
        for x in range(2, 2 + w, 4):
            oled.fill_rect(x, 54, 2, 8, 1)
    else:
        oled.fill_rect(2, 54, w, 8, 1)


def draw_key(label, sub, now):
    if len(label) <= 8 and not sub:
        g["big_text"](label, (128 - 16 * len(label)) // 2, 4, 2)
    else:
        g["center_text"](label[:16], 2)
        for j, l in enumerate(g["wrap"](sub, 16)):
            g["center_text"](l, 14 + j * 10)
    hop = -abs(int(3 * math.sin(now / 120)))
    for cx, side in ((48, -1), (80, 1)):
        g["draw_eye"](cx, 48 + hop, 22, 18, "excited", side)


def draw(now):
    """Volume, microfono o nome del tasto appena premuto, sopra a tutto. True se ha disegnato."""
    if ov["kind"] is None:
        return False
    if time.ticks_diff(now, ov["until"]) > 0:
        ov["kind"] = None
        return False
    if ov["kind"] in ("spk", "mic"):
        draw_level(ov["kind"])
    else:
        draw_key(ov["label"], ov["sub"], now)
    return True


FULL_SCENES = ("code", "game", "osm", "music", "video", "sleep")  # usano anche il fondo dello schermo


def decorate(now):
    """Sopra la faccia: barra in basso con uscita, volume e microfono. Nelle scene che occupano
    tutto lo schermo resta solo il microfono sbarrato, se è spento."""
    if audio["spk"] is not None and cur_mood not in FULL_SCENES:
        oled.fill_rect(0, 54, 128, 10, 0)
        oled.hline(0, 54, 128, 1)
        left = (audio["out"] or "Audio") + " " + ("MUTO" if audio["spkm"] else "%d%%" % audio["spk"])
        oled.text(left[:12], 0, 56)
        if audio["mic"] is not None:
            if audio["micm"]:  # microfono spento: scritta in negativo e sbarrata
                oled.fill_rect(102, 55, 26, 9, 1)
                oled.text("MIC", 103, 56, 0)
                oled.line(102, 63, 127, 55, 0)
            else:
                oled.text("MIC", 104, 56, 1)
        return
    if audio["mic"] is not None and audio["micm"]:
        oled.fill_rect(114, 49, 14, 15, 0)
        g["rrect"](119, 50, 5, 8, 2)
        oled.ellipse(121, 55, 4, 4, 1, False, 0b1100)
        oled.vline(121, 60, 3, 1)
        oled.line(115, 50, 127, 63, 1)
