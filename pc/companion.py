# Desktop Companion - app per Windows (icona nella barra di sistema)
# Trova l'RP2040 sulla USB e gli manda CPU, RAM, ora, attività di Claude e la
# "scena" (cosa stai facendo al PC). Gli hook di Claude Code mandano eventi via
# UDP su 127.0.0.1:47800, vedi notify.py.
import asyncio
import ctypes
import json
import logging
import os
import socket
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import winreg
from ctypes import wintypes
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path

import psutil
import pystray
import serial
from PIL import Image, ImageDraw
from serial.tools import list_ports

import updater
from personality import Personality

try:
    import pynvml  # schede NVIDIA: uso, temperatura e memoria della GPU
except ImportError:
    pynvml = None
try:  # controllo multimediale di Windows: sapere se c'è musica e che brano è
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus)
except ImportError:
    MediaManager = None
try:
    import uiautomation as uia  # per leggere l'indirizzo nel browser (solo su OpenStreetMap)
    uia.SetGlobalSearchTimeout(1)  # se la barra non si trova, non bloccare l'app per 10 s
except ImportError:
    uia = None

PC_DIR = Path(__file__).resolve().parent
ROOT = PC_DIR.parent
FIRMWARE = ROOT / "firmware" / "rp2040" / "main.py"
NOTIFY = PC_DIR / "notify.py"
STATUSLINE = PC_DIR / "statusline.py"
LAUNCHER = ROOT / "Desktop Companion.pyw"
LOG_FILE = ROOT / "companion.log"
CONFIG_FILE = ROOT / "config.json"
ICON_FILE = ROOT / "companion.ico"
CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"
CLAUDE_CREDENTIALS = Path.home() / ".claude" / ".credentials.json"
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"  # lo stesso servizio di /usage
USAGE_EVERY = 300  # secondi tra una lettura dei consumi e l'altra
WEATHER_EVERY = 900  # secondi tra una lettura del meteo e l'altra (Open-Meteo, gratuito)
WEATHER_TEXT = {"storm": "temporale", "snow": "neve", "rain": "pioggia", "cold": "freddo", "hot": "caldo",
                "fog": "nebbia", "sun": "sole", "moon": "sereno", "cloud": "nuvoloso"}
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
APP_NAME = "DesktopCompanion"
PYTHONW = Path(sys.executable).with_name("pythonw.exe")

DEVICE_VIDS = {0x2E8A: "rp2040", 0x303A: "nesso"}  # Raspberry Pi RP2040, Espressif (Nesso N1)
DEVICE_NAMES = {"rp2040": "robot", "nesso": "Nesso N1"}
NESSO_APP_BIN = ROOT / "firmware" / "nesso_n1" / "build" / "NessoN1_Suite.ino.bin"
NESSO_APP_OFFSET = "0x10000"  # solo l'app: bootloader, partizioni e impostazioni salvate restano
UDP_PORT = 47800

# attività di Claude -> per quanti secondi restano valide senza nuovi eventi
ACTIVITIES = {"work": 900, "ask": 600, "done": 5, "none": 0}
HOOK_EVENTS = {"UserPromptSubmit": "work", "PostToolUse": "work", "Notification": "ask", "Stop": "done"}

CODE_APPS = {"code.exe", "code - insiders.exe", "cursor.exe", "windsurf.exe"}
# app che vanno a schermo intero ma non sono giochi
NOT_GAMES = {"explorer.exe", "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "brave.exe",
             "vlc.exe", "powerpnt.exe", "applicationframehost.exe", "searchhost.exe", "lockapp.exe",
             "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe",
             "python.exe", "pythonw.exe", "windowsterminal.exe", "mpc-hc64.exe", "spotify.exe"}
# giochi riconosciuti sempre, anche in finestra: eseguibile -> id mostrato sul robot
GAMES = {"minecraft.windows.exe": "minecraft", "sotgame.exe": "sot", "cities.exe": "cities",
         "cities2.exe": "cities", "eurotrucks2.exe": "ets2"}
GAME_NAMES = {"minecraft": "Minecraft", "sot": "Sea of Thieves", "cities": "Cities: Skylines",
              "ets2": "Euro Truck Simulator", "isonzo": "Isonzo", "tlou": "The Last of Us"}
GAME_IDS = {v: k for k, v in GAME_NAMES.items()}
# riserva: giochi riconosciuti dal titolo della finestra
GAME_TITLES = {"sea of thieves": "sot", "cities: skylines": "cities", "euro truck simulator": "ets2",
               "isonzo": "isonzo", "the last of us": "tlou"}
CONTROLLER_GRACE = 30  # secondi: dopo l'ultimo input del controller resta in modalità gioco
GAME_GRACE = 15  # secondi: se il gioco perde il primo piano per poco (overlay, notifiche) la scena resta
BROWSERS = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe"}
# parole nel titolo della pagina di osm.org che indicano l'editor (iD) invece della consultazione
OSM_EDIT_WORDS = ("modifica", "edit", "rapid")

ICON_COLORS = {"off": (110, 110, 110), "on": (0, 150, 170), "work": (140, 70, 200),
               "ask": (240, 140, 0), "done": (40, 170, 70)}
STATUS_TEXT = {"off": "In attesa del companion...", "on": "Collegato"}
ACTIVITY_TEXT = {"none": "in pausa", "work": "sta pensando", "ask": "ti sta cercando", "done": "ha finito"}
SCENE_TEXT = {"none": "niente di speciale", "code": "programmi in VS Code", "game": "stai giocando",
              "osm": "sei su OpenStreetMap", "music": "ascolti musica", "video": "guardi un video"}
MUSIC_EVERY = 2  # secondi tra un controllo della musica e l'altro
OSM_TEXT = {"view": "consulti OpenStreetMap", "edit": "modifichi OpenStreetMap"}

log = logging.getLogger("companion")


# ---------------------------------------------------------------- cosa c'è aperto

user32 = ctypes.windll.user32


class _MonitorInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


class _XInputGamepad(ctypes.Structure):
    _fields_ = [("wButtons", wintypes.WORD), ("bLeftTrigger", ctypes.c_ubyte),
                ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short), ("sThumbRY", ctypes.c_short)]


class _XInputState(ctypes.Structure):
    _fields_ = [("dwPacketNumber", wintypes.DWORD), ("Gamepad", _XInputGamepad)]


try:
    _xinput = ctypes.windll.xinput1_4
except OSError:
    _xinput = None


def foreground_app():
    """Eseguibile in primo piano, titolo della finestra e se è a schermo intero."""
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return "", "", False, None
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    try:
        name = psutil.Process(pid.value).name().lower()
    except psutil.Error:
        return "", "", False, None
    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title, 256)
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    info = _MonitorInfo(cbSize=ctypes.sizeof(_MonitorInfo))
    user32.GetMonitorInfoW(user32.MonitorFromWindow(hwnd, 2), ctypes.byref(info))
    m = info.rcMonitor
    fullscreen = (rect.left <= m.left and rect.top <= m.top
                  and rect.right >= m.right and rect.bottom >= m.bottom)
    return name, title.value, fullscreen, hwnd


def detect_game(name, title):
    if name in GAMES:
        return GAMES[name]
    if name.startswith("isonzo"):  # Isonzo.exe / Isonzo-Win64-Shipping.exe
        return "isonzo"
    if name.startswith("tlou"):  # tlou-i.exe, tlou-ii.exe...
        return "tlou"
    t = title.lower()
    if name in ("javaw.exe", "java.exe") and t.startswith("minecraft"):
        return "minecraft"  # Minecraft Java Edition
    if name not in BROWSERS and name not in CODE_APPS:  # es. un video "Sea of Thieves" su YouTube non conta
        for words, game in GAME_TITLES.items():
            if t.startswith(words):
                return game
    return None


class BrowserUrl:
    """Indirizzo della scheda attiva del browser, letto dalla barra degli indirizzi con
    UI Automation. Va usato sempre dallo stesso thread (quello che chiama setup)."""

    def __init__(self):
        self.hwnd = None
        self.bar = None
        self.init = None

    def setup(self):
        if uia:
            self.init = uia.UIAutomationInitializerInThread()

    def read(self, hwnd):
        if not uia or not hwnd:
            return ""
        try:
            if hwnd != self.hwnd or self.bar is None:
                self.hwnd = hwnd
                self.bar = uia.ControlFromHandle(hwnd).EditControl(searchDepth=12)
            return self.bar.GetValuePattern().Value.lower()
        except Exception as e:  # finestra chiusa, barra nascosta (schermo intero)...
            log.debug("indirizzo non leggibile: %s", e)
            self.bar = None
            return ""


def detect_osm(name, title, read_url=lambda: ""):
    """"view" o "edit" se in primo piano c'è OpenStreetMap (osm.org nel browser, o JOSM).
    Nell'editor iD il titolo resta "OpenStreetMap", quindi guardo l'indirizzo (/edit),
    che viene letto solo in questo caso."""
    t = title.lower()
    if "java openstreetmap editor" in t or name.startswith("josm"):
        return "edit"
    if name in BROWSERS and ("openstreetmap" in t or "osm.org" in t):
        url = read_url()
        if "openstreetmap.org" in url or "osm.org" in url:
            return "edit" if "/edit" in url else "view"
        return "edit" if any(w in t for w in OSM_EDIT_WORDS) else "view"
    return None


class _LastInput(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]


def idle_seconds():
    """Secondi dall'ultimo uso di mouse o tastiera."""
    li = _LastInput(cbSize=ctypes.sizeof(_LastInput))
    user32.GetLastInputInfo(ctypes.byref(li))
    return (ctypes.windll.kernel32.GetTickCount() - li.dwTime) / 1000


def load_config():
    try:
        return json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    CONFIG_FILE.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


class ControllerWatcher:
    """Si accorge quando un controller Xbox/XInput viene usato."""

    def __init__(self):
        self.packets = {}
        self.last_input = 0.0

    def poll(self):
        if not _xinput:
            return False
        state = _XInputState()
        for i in range(4):
            if _xinput.XInputGetState(i, ctypes.byref(state)) != 0:
                continue
            if self.packets.get(i) not in (None, state.dwPacketNumber):
                self.last_input = time.time()
            self.packets[i] = state.dwPacketNumber
        return time.time() - self.last_input < CONTROLLER_GRACE


class GpuReader:
    """Uso %, temperatura °C e memoria video % della prima GPU NVIDIA (None se non c'è)."""

    def __init__(self):
        self.handle = None
        if pynvml:
            try:
                pynvml.nvmlInit()
                self.handle = pynvml.nvmlDeviceGetHandleByIndex(0)
            except pynvml.NVMLError as e:
                log.info("GPU non disponibile: %s", e)

    def read(self):
        if not self.handle:
            return None
        try:
            util = pynvml.nvmlDeviceGetUtilizationRates(self.handle).gpu
            temp = pynvml.nvmlDeviceGetTemperature(self.handle, pynvml.NVML_TEMPERATURE_GPU)
            mem = pynvml.nvmlDeviceGetMemoryInfo(self.handle)
            return util, temp, round(mem.used / mem.total * 100)
        except pynvml.NVMLError:
            return None


# ---------------------------------------------------------------- hook di Claude Code

def _hook_text(h):
    return (h.get("command", "") + " " + " ".join(h.get("args", []))).replace("\\", "/").lower()


def _is_ours(entry):
    return any(NOTIFY.as_posix().lower() in _hook_text(h) for h in entry.get("hooks", []))


def _load_claude_settings():
    try:
        return json.loads(CLAUDE_SETTINGS.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}


def hooks_installed():
    try:
        s = _load_claude_settings()
    except (OSError, ValueError):
        return False
    return any(_is_ours(e) for e in s.get("hooks", {}).get("Stop", []))


def set_hooks(enabled):
    s = _load_claude_settings()  # se il JSON è rovinato solleva: meglio non sovrascriverlo
    hooks = s.setdefault("hooks", {})
    for event, activity in HOOK_EVENTS.items():
        entries = [e for e in hooks.get(event, []) if not _is_ours(e)]
        if enabled:
            entries.append({"hooks": [{"type": "command", "command": PYTHONW.as_posix(),
                                       "args": [NOTIFY.as_posix(), activity],
                                       "async": True, "timeout": 5}]})
        if entries:
            hooks[event] = entries
        else:
            hooks.pop(event, None)
    if not hooks:
        s.pop("hooks")
    # barra di stato con i consumi: la installa solo se non ce n'è già un'altra
    ours = STATUSLINE.as_posix().lower() in s.get("statusLine", {}).get("command", "").replace("\\", "/").lower()
    if enabled and ("statusLine" not in s or ours):
        python = PYTHONW.with_name("python.exe").as_posix()  # serve la console per stampare
        s["statusLine"] = {"type": "command", "command": f'"{python}" "{STATUSLINE.as_posix()}"'}
    elif not enabled and ours:
        s.pop("statusLine")
    CLAUDE_SETTINGS.parent.mkdir(exist_ok=True)
    tmp = CLAUDE_SETTINGS.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(s, indent=2), encoding="utf-8")
    os.replace(tmp, CLAUDE_SETTINGS)


# ---------------------------------------------------------------- consumi di Claude

def fetch_usage():
    """Consumi 5 ore / settimana, letti col login che Claude Code salva sul PC.
    Il token viene mandato solo ad api.anthropic.com. Restituisce
    [perc_5h, reset_5h, perc_7g, reset_7g] come per Hub.set_usage, o None."""
    oauth = json.loads(CLAUDE_CREDENTIALS.read_text(encoding="utf-8")).get("claudeAiOauth") or {}
    token = oauth.get("accessToken")
    if not token or oauth.get("expiresAt", 0) / 1000 < time.time():
        return None  # scaduto: Claude Code lo rinnova da solo al prossimo utilizzo
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": "Bearer " + token, "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": "desktop-companion"})
    with urllib.request.urlopen(req, timeout=15) as r:
        data = json.load(r)
    parts = []
    for name in ("five_hour", "seven_day"):
        w = data.get(name) or {}
        perc, reset = w.get("utilization"), w.get("resets_at")
        parts.append("-" if perc is None else str(round(perc)))
        parts.append(str(int(datetime.fromisoformat(reset).timestamp())) if reset else "-")
    return parts


# ---------------------------------------------------------------- musica

MUSIC_APPS = ("spotify", "music", "deezer", "tidal", "itunes", "foobar", "musicbee", "winamp", "aimp")
MUSIC_WORDS = ("official video", "official music video", "official audio", "videoclip", "music video",
               "lyric", "testo", "audio", "remix", "feat", "ft.", "acoustic", "unplugged", "karaoke")


def is_music(app, artist, title, album):
    """Musica o video normale? App di musica, album (YouTube Music), canali "VEVO"/"- Topic"
    e parole tipiche nel titolo dei video musicali."""
    if any(k in app.lower() for k in MUSIC_APPS) or album:
        return True
    ar, t = artist.lower(), title.lower()
    return ar.endswith(" - topic") or "vevo" in ar or any(w in t for w in MUSIC_WORDS)


async def _now_playing():
    mgr = await MediaManager.request_async()
    for s in mgr.get_sessions():
        if s.get_playback_info().playback_status != PlaybackStatus.PLAYING:
            continue
        props = await s.try_get_media_properties_async()
        artist, title, album = props.artist or "", props.title or "", props.album_title or ""
        pos = dur = 0
        try:  # posizione nel brano: l'ultima nota + il tempo passato da allora
            tl = s.get_timeline_properties()
            dur = int((tl.end_time - tl.start_time).total_seconds())
            since = (datetime.now(timezone.utc) - tl.last_updated_time).total_seconds()
            pos = int(tl.position.total_seconds() + max(0, since))
            pos = min(pos, dur) if dur > 0 else 0
        except (AttributeError, TypeError, ValueError, OSError):
            pass
        app = s.source_app_user_model_id or ""
        return {"artist": artist, "title": title, "pos": pos, "dur": max(0, dur),
                "music": is_music(app, artist, title, album)}
    return None


def now_playing():
    """Cosa sta suonando (Spotify, YouTube, ...): artista, titolo, posizione, durata e se è
    musica o un video normale. None se non c'è niente in riproduzione."""
    if not MediaManager:
        return None
    return asyncio.run(_now_playing())


def ascii_text(s, limit=40):
    """Il display ha solo caratteri ASCII: "Perché" -> "Perche"."""
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return " ".join(s.split())[:limit]


# ---------------------------------------------------------------- meteo (Open-Meteo)

def _get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "desktop-companion"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.load(r)


def geocode(city):
    """(lat, lon, nome) della città, o None se non la trova."""
    q = urllib.parse.urlencode({"name": city, "count": 1, "language": "it"})
    res = _get_json("https://geocoding-api.open-meteo.com/v1/search?" + q).get("results")
    if not res:
        return None
    return res[0]["latitude"], res[0]["longitude"], res[0]["name"]


def fetch_weather(lat, lon):
    q = urllib.parse.urlencode({"latitude": lat, "longitude": lon,
                                "current": "temperature_2m,weather_code,is_day"})
    cur = _get_json("https://api.open-meteo.com/v1/forecast?" + q)["current"]
    return cur["weather_code"], round(cur["temperature_2m"]), bool(cur["is_day"])


def weather_kind(code, temp, is_day):
    """Codice meteo WMO + temperatura -> animazione del robot."""
    if code >= 95:
        return "storm"
    if 71 <= code <= 77 or code in (85, 86):
        return "snow"
    if 51 <= code <= 67 or 80 <= code <= 82:
        return "rain"
    if temp <= 5:
        return "cold"
    if temp >= 29:
        return "hot"
    if code in (45, 48):
        return "fog"
    if code in (0, 1):
        return "sun" if is_day else "moon"
    return "cloud"


def ask_city(current, on_done):
    """Finestrella per scrivere la città (in un thread a parte, per non bloccare l'icona)."""
    def run():
        import tkinter as tk
        from tkinter import simpledialog
        root = tk.Tk()
        root.withdraw()
        root.attributes("-topmost", True)
        city = simpledialog.askstring("Desktop Companion", "In che città sei? (per il meteo)",
                                      initialvalue=current or "", parent=root)
        root.destroy()
        if city and city.strip():
            on_done(city.strip())
    threading.Thread(target=run, daemon=True).start()


# ---------------------------------------------------------------- avvio con Windows

def autostart_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, APP_NAME)
        return True
    except OSError:
        return False


def set_autostart(enabled):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if enabled:
            winreg.SetValueEx(k, APP_NAME, 0, winreg.REG_SZ, f'"{PYTHONW}" "{LAUNCHER}"')
        else:
            try:
                winreg.DeleteValue(k, APP_NAME)
            except FileNotFoundError:
                pass


# ---------------------------------------------------------------- hub

def find_ports():
    """Dispositivi companion collegati: {porta: tipo}, tipo = "rp2040" o "nesso"."""
    return {p.device: DEVICE_VIDS[p.vid] for p in list_ports.comports() if p.vid in DEVICE_VIDS}


def open_port(device):
    # DTR/RTS spenti: sull'ESP32 (Nesso N1) aprire la porta con DTR/RTS attivi lo riavvia
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = device, 115200, 1, 2
    s.dtr = s.rts = False
    s.open()
    return s


def make_icon(color, size=64):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / 64  # disegnata su una griglia 64x64, scalata
    d.rounded_rectangle((2 * s, 8 * s, 62 * s, 56 * s), 12 * s, fill=color)
    d.rounded_rectangle((14 * s, 20 * s, 28 * s, 42 * s), 5 * s, fill="white")
    d.rounded_rectangle((36 * s, 20 * s, 50 * s, 42 * s), 5 * s, fill="white")
    return img


def create_desktop_shortcut():
    """Collegamento "Desktop Companion" sul desktop, con l'icona del robottino:
    serve a riavviare l'app se è stata chiusa."""
    # Niente PowerShell: gli antivirus scambiano "powershell -Command ... WScript.Shell" per un
    # malware. Uso lo stesso oggetto COM direttamente da Python (comtypes, già tra le dipendenze).
    import comtypes.client
    make_icon(ICON_COLORS["on"], 256).save(ICON_FILE, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (256, 256)])
    shell = comtypes.client.CreateObject("WScript.Shell", dynamic=True)
    desktop = shell.SpecialFolders("Desktop")
    link = shell.CreateShortcut(str(Path(desktop) / "Desktop Companion.lnk"))
    link.TargetPath = str(PYTHONW)
    link.Arguments = f'"{LAUNCHER}"'
    link.WorkingDirectory = str(ROOT)
    link.IconLocation = str(ICON_FILE)
    link.Description = "Avvia Desktop Companion"
    link.Save()
    log.info("collegamento sul desktop creato")


class Hub:
    def __init__(self):
        self.activity = "none"
        self.activity_until = 0.0
        self.scene = "none"
        self.detail = ""  # gioco riconosciuto, o "view"/"edit" per OpenStreetMap
        self.game_seen = 0.0  # ultima volta che un gioco era in primo piano
        self.cfg = load_config()
        self.personality = Personality(chatter=self.cfg.get("chatter", True))
        self.emo = "none"  # umore di fondo deciso dalla personalità
        self.next_msg_at = 0.0
        self.weather = None  # (animazione, temperatura, nome città)
        self.weather_refresh = threading.Event()
        self.weather_test = None  # animazione meteo da provare subito (menu)
        self.media = None  # cosa sta suonando (vedi now_playing), o None
        self.media_test = ("", 0.0)  # prova dal menu: "music" o "video", e fino a quando
        self.usage = {}  # consumi Claude: {"five_hour": (perc, reset_ts), "seven_day": (...)}
        self.usage_fetched = 0.0
        self.goto_page = None  # pagina da mostrare sul robot, scelta dal menu
        self.updates = {}  # aggiornamenti disponibili su GitHub (vedi updater.pending)
        self.gpu = GpuReader()
        self.pc = (0, 0, None)  # ultimi valori letti: cpu %, ram %, (gpu %, temp, vram %)
        self.usage_refresh = threading.Event()
        self.scene_test = ("none", "", 0.0)  # scena forzata dal menu "Prova"
        self.port = None
        self.icon = None
        self.icon_key = None
        self.changed = threading.Event()
        self.paused = threading.Event()  # impostato mentre si aggiorna il firmware
        self.controller = ControllerWatcher()
        self.browser_url = BrowserUrl()

    # --- stato
    def set_activity(self, name):
        self.activity = name
        self.activity_until = time.time() + ACTIVITIES[name]
        self.changed.set()

    def show_page(self, n):
        self.goto_page = n
        self.changed.set()

    def test_scene(self, name, detail=""):
        self.scene_test = (name, detail, time.time() + 10)
        self.changed.set()

    def set_city(self, city):
        self.cfg["city"] = city
        save_config(self.cfg)
        self.weather = None
        self.weather_refresh.set()

    def test_weather(self, kind):
        self.weather_test = kind
        self.changed.set()

    def weather_text(self):
        if not self.cfg.get("city"):
            return "scegli la città (Cambia città...)"
        if not self.weather:
            return self.cfg["city"] + ": in attesa..."
        kind, temp, name = self.weather
        return f"{name} {temp}°C, {WEATHER_TEXT[kind]}"

    @property
    def music(self):
        return bool(self.media and self.media["music"])

    def media_name(self):
        m = self.media
        return f"{m['artist']} - {m['title']}" if m["artist"] and m["title"] else m["artist"] or m["title"]

    def music_loop(self):
        while True:
            try:
                playing = now_playing()
            except Exception as e:  # errori WinRT: nessuna musica, riprova tra poco
                log.debug("musica non leggibile: %s", e)
                playing = None
            kind, until = self.media_test
            if time.time() < until:  # prova dal menu
                left = int(until - time.time())
                playing = {"artist": "Desktop Companion", "title": "Prova " + ("musica" if kind == "music" else "video"),
                           "pos": 15 - left, "dur": 15, "music": kind == "music"}
            before = (self.media["title"], self.media["music"]) if self.media else None
            self.media = playing
            if (playing and (playing["title"], playing["music"])) != before:
                log.info("media: %s", f"{'musica' if self.music else 'video'}: {self.media_name()}" if playing else "fermo")
                self.changed.set()
            time.sleep(MUSIC_EVERY)

    def test_media(self, kind):
        self.media_test = (kind, time.time() + 15)

    def music_line(self):
        """H <1 musica | 0 video | - niente>, N <titolo>, B <posizione> <durata> (secondi, 0 0 se non si sa)."""
        m = self.media
        if not m:
            return "H -\n"
        lines = "H %d\nB %d %d\n" % (self.music, m["pos"], m["dur"])
        name = ascii_text(self.media_name())
        if name:
            lines += f"N {name}\n"
        return lines

    def weather_loop(self):
        place = None  # (lat, lon, nome) della città in config
        while True:
            city = self.cfg.get("city")
            try:
                if city and (not place or place[3] != city):
                    found = geocode(city)
                    place = found + (city,) if found else None
                    if not found:
                        log.info("meteo: città non trovata: %s", city)
                if place:
                    code, temp, is_day = fetch_weather(place[0], place[1])
                    kind = weather_kind(code, temp, is_day)
                    if not self.weather or self.weather[:2] != (kind, temp):
                        log.info("meteo %s: %s°C, %s (codice %s)", place[2], temp, WEATHER_TEXT[kind], code)
                    self.weather = (kind, temp, place[2])
                    self.changed.set()
            except (OSError, ValueError, KeyError) as e:
                log.info("meteo non disponibile: %s", e)
            self.weather_refresh.wait(WEATHER_EVERY)
            self.weather_refresh.clear()

    def set_chatter(self, enabled):
        self.personality.chatter = self.cfg["chatter"] = enabled
        save_config(self.cfg)

    def update_state(self):
        if self.activity != "none" and time.time() > self.activity_until:
            self.activity = "none"
        before = (self.scene, self.detail)
        name, title, fullscreen, hwnd = foreground_app()
        controller = self.controller.poll()
        test, test_detail, until = self.scene_test
        if time.time() < until:
            self.scene, self.detail = test, test_detail
        else:
            game = detect_game(name, title)
            osm = detect_osm(name, title, lambda: self.browser_url.read(hwnd))
            recent_game = before[0] == "game" and time.time() - self.game_seen < GAME_GRACE
            if game:
                self.scene, self.detail = "game", game
                self.game_seen = time.time()
            elif controller or (fullscreen and name not in NOT_GAMES):
                # gioco generico, oppure un popup (es. permessi di Windows) sopra un gioco conosciuto
                self.scene, self.detail = "game", self.detail if recent_game else ""
            elif osm:
                self.scene, self.detail = "osm", osm
            elif name in CODE_APPS:
                self.scene, self.detail = "code", ""
            elif recent_game:
                pass  # il gioco ha perso il primo piano solo per un attimo: la scena resta
            elif self.media:
                self.scene, self.detail = ("music" if self.music else "video"), ""
            else:
                self.scene, self.detail = "none", ""
        if (self.scene, self.detail) != before:
            log.info("scena: %s %s (in primo piano: %s%s)", self.scene, self.detail, name or "?",
                     ", schermo intero" if fullscreen else "")
        # il controller non conta come "uso del PC" per Windows: lo aggiungo io
        idle = min(idle_seconds(), time.time() - self.controller.last_input)
        self.emo = self.personality.tick(self.scene, self.detail, idle, fullscreen,
                                         self.pc[0], self.pc[2], self.current_usage(),
                                         self.weather[0] if self.weather else None, bool(self.media))
        if self.personality.queue:
            self.changed.set()
        self.refresh_icon()

    def scene_text(self):
        if self.scene == "game" and self.detail:
            return "stai giocando a " + GAME_NAMES[self.detail]
        if self.scene == "osm":
            return OSM_TEXT[self.detail]
        text = SCENE_TEXT[self.scene]
        if self.media:
            text += (": " if self.scene in ("music", "video") else ", con musica: ") + self.media_name()
        return text

    def message_line(self):
        """Una frase alla volta, distanziate di qualche secondo: M <emozione> <frase>."""
        if time.time() < self.next_msg_at:
            return ""
        msg = self.personality.pop_message()
        if not msg:
            return ""
        self.next_msg_at = time.time() + 6
        log.info("il robot dice: %s (%s)", msg[1], msg[0])
        return f"M {msg[0]} {msg[1][:40]}\n"

    def set_usage(self, parts):
        """parts = [perc_5h, reset_5h, perc_7g, reset_7g], "-" se mancante."""
        vals = [None if p == "-" else int(p) for p in parts]
        before = dict(self.usage)
        for name, (perc, reset) in (("five_hour", vals[:2]), ("seven_day", vals[2:])):
            if perc is not None:
                self.usage[name] = (perc, reset)
        if self.usage != before:
            log.info("consumi Claude: %s", self.usage_text())
        self.changed.set()

    def current_usage(self):
        now = time.time()
        for name in list(self.usage):  # finestra scaduta: il consumo è ripartito da zero
            reset = self.usage[name][1]
            if reset and now > reset:
                del self.usage[name]
        return self.usage

    def usage_text(self):
        u = self.current_usage()
        if not u:
            return "nessun dato (usa Claude Code)"
        parts = []
        if "five_hour" in u:
            parts.append(f"5 ore {u['five_hour'][0]}%")
        if "seven_day" in u:
            parts.append(f"settimana {u['seven_day'][0]}%")
        return ", ".join(parts)

    def usage_line(self):
        """Riga per il firmware: U <perc5h> <perc7g> <reset5h> <reset7g>, "-" se manca."""
        u = self.current_usage()
        days = ("lun", "mar", "mer", "gio", "ven", "sab", "dom")
        p5, r5 = u.get("five_hour", (None, None))
        p7, r7 = u.get("seven_day", (None, None))
        if r5:
            mins = max(0, int((r5 - time.time()) // 60))
            r5 = f"tra_{mins // 60}h{mins % 60:02d}"
        if r7:
            t = time.localtime(r7)
            r7 = f"{days[t.tm_wday]}_{t.tm_hour:02d}:{t.tm_min:02d}"
        return "U " + " ".join("-" if v is None else str(v) for v in (p5, p7, r5, r7)) + "\n"

    def pc_text(self):
        cpu, ram, gpu = self.pc
        text = f"CPU {cpu}%, RAM {ram}%"
        if gpu:
            text += f", GPU {gpu[0]}% ({gpu[1]}°C)"
        return text

    def status_lines(self):
        cpu = round(psutil.cpu_percent(None))
        ram = round(psutil.virtual_memory().percent)
        gpu = self.gpu.read()
        self.pc = (cpu, ram, gpu)
        video = "V %d %d %d\n" % gpu if gpu else "V - - -\n"
        goto = ""
        if self.goto_page is not None:
            goto, self.goto_page = f"G {self.goto_page}\n", None
        goto += "W %s %d\n" % self.weather[:2] if self.weather else "W - -\n"
        if self.weather_test:
            goto, self.weather_test = goto + f"T {self.weather_test}\n", None
        return (f"S {cpu} {ram} {time.strftime('%H:%M')}\n"
                f"A {self.activity}\nP {self.scene} {self.detail}\nE {self.emo}\n"
                + video + self.usage_line() + self.music_line() + goto + self.message_line()).encode()

    # --- icona
    def refresh_icon(self):
        key = "off" if not self.port else self.activity if self.activity != "none" else "on"
        if self.icon and key != self.icon_key:
            self.icon_key = key
            self.icon.icon = make_icon(ICON_COLORS[key])
            self.icon.title = "Desktop Companion - " + self.status_text()
        if self.icon:
            self.icon.update_menu()

    def status_text(self):
        return f"Collegato: {self.port}" if self.port else STATUS_TEXT["off"]

    # --- thread
    def udp_loop(self, sock):
        while True:
            data, _ = sock.recvfrom(256)
            name = data.decode(errors="ignore").strip().lower()
            parts = name.split()
            if len(parts) == 5 and parts[0] == "usage":
                try:
                    self.set_usage(parts[1:])
                except ValueError:
                    pass
            elif name in ACTIVITIES:
                log.info("evento Claude: %s", name)
                self.set_activity(name)
                # Claude ha appena finito: i consumi sono cambiati, rileggili (max 1 volta al minuto)
                if name == "done" and time.time() - self.usage_fetched > 60:
                    self.usage_refresh.set()

    def usage_loop(self):
        while True:
            self.usage_fetched = time.time()
            try:
                parts = fetch_usage()
                if parts:
                    self.set_usage(parts)
            except (OSError, ValueError, KeyError) as e:
                log.info("consumi non disponibili: %s", e)
            self.usage_refresh.wait(USAGE_EVERY)
            self.usage_refresh.clear()

    def serial_loop(self):
        """Tiene aperti tutti i dispositivi collegati (robot RP2040, Nesso N1) e manda
        a tutti gli stessi dati, ogni secondo o subito quando succede qualcosa."""
        self.browser_url.setup()  # update_state gira in questo thread
        psutil.cpu_percent(None)
        links = {}  # porta -> (serial, tipo)
        last_scan = 0.0
        while True:
            if self.paused.is_set():  # aggiornamento firmware in corso: porte libere
                for s, _ in links.values():
                    s.close()
                links.clear()
                self.port = None
                time.sleep(0.5)
                continue
            if time.time() - last_scan > 2:
                last_scan = time.time()
                found = find_ports()
                for dev, kind in found.items():
                    if dev not in links:
                        try:
                            links[dev] = (open_port(dev), kind)
                            log.info("collegato: %s su %s", DEVICE_NAMES[kind], dev)
                        except (serial.SerialException, OSError) as e:
                            log.debug("non riesco ad aprire %s: %s", dev, e)
                for dev in [d for d in links if d not in found]:
                    links.pop(dev)[0].close()
                    log.info("scollegato: %s", dev)
                self.port = ", ".join(f"{DEVICE_NAMES[k]} ({d})" for d, (_, k) in links.items()) or None
            self.update_state()
            if links:
                payload = self.status_lines()
                for dev, (s, kind) in list(links.items()):
                    try:
                        s.write(payload)
                        if s.in_waiting:  # comandi dal dispositivo (righe che iniziano con "!")
                            for line in s.read(s.in_waiting).decode(errors="ignore").splitlines():
                                if line.strip().startswith("!"):
                                    self.device_command(line.strip(), kind)
                    except (serial.SerialException, OSError) as e:
                        log.info("scollegato: %s su %s (%s)", DEVICE_NAMES[kind], dev, e)
                        s.close()
                        links.pop(dev)
            self.changed.wait(1)
            self.changed.clear()

    def say(self, emo, text):
        """Fa dire subito una frase ai dispositivi."""
        self.personality.queue.insert(0, (emo, text))
        self.next_msg_at = 0
        self.changed.set()

    def device_command(self, cmd, kind):
        log.info("comando da %s: %s", DEVICE_NAMES[kind], cmd)
        if cmd == "!update":  # KEY2 tenuto premuto sul Nesso: aggiorna da GitHub
            def job():
                self.say("excited", "Cerco aggiornamenti...")
                self.check_updates(quiet=True)
                if self.updates:
                    self.say("excited", "Aggiorno: " + self.updates_text())
                    self.apply_updates()
                else:
                    self.say("happy", "Tutto aggiornato!")
            threading.Thread(target=job, daemon=True).start()

    # --- aggiornamenti da GitHub
    def update_loop(self):
        time.sleep(60)
        while True:
            self.check_updates(quiet=True)
            time.sleep(12 * 3600)

    def check_updates(self, quiet=False):
        try:
            self.updates = updater.pending(ROOT, self.cfg.get("fw", {}))
        except (OSError, ValueError) as e:
            log.info("controllo aggiornamenti non riuscito: %s", e)
            if not quiet:
                self.icon.notify("Non riesco a contattare GitHub.", "Aggiornamenti")
            return
        if self.updates:
            log.info("aggiornamenti disponibili: %s", self.updates)
            self.icon.notify("Aggiornamento disponibile: " + self.updates_text() + "\nMenu > Aggiorna ora",
                             "Desktop Companion")
        elif not quiet:
            self.icon.notify("È tutto aggiornato.", "Aggiornamenti")
        self.icon.update_menu()

    def updates_text(self):
        names = {"app": "app", "rp2040": "robot", "nesso": "Nesso N1"}
        return ", ".join(f"{names[k]} {new}" for k, (old, new) in self.updates.items())

    def apply_updates(self):
        def job():
            ups = dict(self.updates)
            if not ups:
                self.check_updates()
                return
            self.icon.notify("Scarico l'aggiornamento da GitHub...", "Desktop Companion")
            try:
                src = updater.download(updater.new_temp_dir())
            except OSError as e:
                log.info("download aggiornamento fallito: %s", e)
                self.icon.notify("Download non riuscito.", "Aggiornamenti")
                return
            fw_kinds = {k for k in ups if k in ("rp2040", "nesso")}
            if fw_kinds:
                remote = updater.local_versions(src)
                for kind, ok in self.flash_devices(src, fw_kinds):
                    if ok:
                        self.cfg.setdefault("fw", {})[kind] = remote.get(kind, "")
                save_config(self.cfg)
            if "app" in ups:
                updater.install_app(src, ROOT)
                subprocess.run([str(PYTHONW), "-m", "pip", "install", "--user", "-q", "-r",
                                str(ROOT / "pc" / "requirements.txt")],
                               creationflags=subprocess.CREATE_NO_WINDOW, timeout=600)
                log.info("app aggiornata a %s: riavvio", ups["app"][1])
                self.restart()
            self.updates = {}
            self.icon.notify("Aggiornamento completato.", "Desktop Companion")
            self.icon.update_menu()
        threading.Thread(target=job, daemon=True).start()

    def restart(self):
        """Riavvia l'app: la nuova parte fra 3 s, quando questa ha liberato la porta UDP."""
        subprocess.Popen([str(PYTHONW), "-c",
                          f"import time,subprocess; time.sleep(3); subprocess.Popen([r'{PYTHONW}', r'{LAUNCHER}'])"],
                         creationflags=subprocess.DETACHED_PROCESS)
        self.icon.stop()
        os._exit(0)

    # --- azioni del menu
    def flash_devices(self, root, kinds=None):
        """Scrive il firmware sui dispositivi collegati (solo i tipi in kinds, se dato).
        Restituisce [(tipo, riuscito)]."""
        root = Path(root)

        def flash(dev, kind):
            if kind == "rp2040":
                cmd = [str(PYTHONW), "-m", "mpremote", "connect", dev,
                       "cp", str(root / "firmware" / "rp2040" / "main.py"), ":main.py", "+", "reset"]
            else:
                app_bin = root / NESSO_APP_BIN.relative_to(ROOT)
                if not app_bin.exists():
                    log.info("firmware Nesso non trovato: %s", app_bin)
                    return False
                cmd = [str(PYTHONW), "-m", "esptool", "--chip", "esp32c6", "--port", dev, "--baud", "921600",
                       "write-flash", NESSO_APP_OFFSET, str(app_bin)]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=180,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            log.info("firmware %s su %s: codice %s %s", DEVICE_NAMES[kind], dev, r.returncode,
                     (r.stdout + r.stderr)[-400:])
            return r.returncode == 0

        devices = {d: k for d, k in find_ports().items() if not kinds or k in kinds}
        results = []
        if not devices:
            return results
        self.paused.set()
        self.changed.set()
        try:
            time.sleep(1.5)  # lascia chiudere le porte al thread seriale
            for dev, kind in devices.items():
                try:
                    results.append((kind, flash(dev, kind)))
                except (OSError, subprocess.SubprocessError) as e:
                    log.exception("aggiornamento %s fallito: %s", dev, e)
                    results.append((kind, False))
        finally:
            self.paused.clear()
        return results

    def update_firmware(self):
        """Menu: riscrive il firmware (quello nella cartella dell'app) su tutti i dispositivi."""
        def job():
            results = self.flash_devices(ROOT)
            if not results:
                self.icon.notify("Nessun dispositivo collegato.", "Aggiornamento firmware")
                return
            text = ", ".join(f"{DEVICE_NAMES[k]} {'ok' if ok else 'ERRORE'}" for k, ok in results)
            self.icon.notify("Firmware: " + text, "Desktop Companion")
        threading.Thread(target=job, daemon=True).start()


def build_menu(hub):
    def toggle_hooks(icon, item):
        try:
            set_hooks(not hooks_installed())
        except (OSError, ValueError) as e:
            icon.notify(f"Non riesco a modificare le impostazioni di Claude Code: {e}", "Desktop Companion")

    def toggle_autostart(icon, item):
        set_autostart(not autostart_enabled())

    return pystray.Menu(
        pystray.MenuItem(lambda item: "Stato: " + hub.status_text(), None, enabled=False),
        pystray.MenuItem(lambda item: "Claude: " + ACTIVITY_TEXT[hub.activity], None, enabled=False),
        pystray.MenuItem(lambda item: "Tu: " + hub.scene_text(), None, enabled=False),
        pystray.MenuItem(lambda item: "PC: " + hub.pc_text(), None, enabled=False),
        pystray.MenuItem(lambda item: "Consumi Claude: " + hub.usage_text(), None, enabled=False),
        pystray.MenuItem(lambda item: "Meteo: " + hub.weather_text(), None, enabled=False),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Prova animazione", pystray.Menu(
            pystray.MenuItem("Claude pensa", lambda: hub.set_activity("work")),
            pystray.MenuItem("Claude ti cerca", lambda: hub.set_activity("ask")),
            pystray.MenuItem("Claude ha finito", lambda: hub.set_activity("done")),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Programma in VS Code", lambda: hub.test_scene("code")),
            pystray.MenuItem("OpenStreetMap: consulta", lambda: hub.test_scene("osm", "view")),
            pystray.MenuItem("OpenStreetMap: modifica", lambda: hub.test_scene("osm", "edit")),
            pystray.MenuItem("Dimmi qualcosa", lambda: (hub.personality.say("chat"), hub.changed.set())),
            pystray.MenuItem("Ascolta musica", lambda: hub.test_media("music")),
            pystray.MenuItem("Guarda un video", lambda: hub.test_media("video")),
            pystray.MenuItem("Meteo", pystray.Menu(
                *[pystray.MenuItem(text.capitalize(), (lambda k: lambda: hub.test_weather(k))(kind))
                  for kind, text in WEATHER_TEXT.items()],
            )),
            pystray.MenuItem("Gioca", pystray.Menu(
                pystray.MenuItem("Gioco qualsiasi", lambda: hub.test_scene("game")),
                *[pystray.MenuItem(label, lambda icon, item: hub.test_scene("game", GAME_IDS[item.text]))
                  for label in GAME_NAMES.values()],
            )),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Torna normale", lambda: (hub.set_activity("none"), hub.test_scene("none"))),
        )),
        pystray.MenuItem("Mostra sul robot", pystray.Menu(
            pystray.MenuItem("Consumi Claude", lambda: hub.show_page(1)),
            pystray.MenuItem("Orologio", lambda: hub.show_page(2)),
            pystray.MenuItem("Occhi", lambda: hub.show_page(0)),
        )),
        pystray.MenuItem("Cambia città...", lambda: ask_city(hub.cfg.get("city"), hub.set_city)),
        pystray.MenuItem("Chiacchiere", lambda: hub.set_chatter(not hub.personality.chatter),
                         checked=lambda item: hub.personality.chatter),
        pystray.MenuItem("Reagisci a Claude Code", toggle_hooks, checked=lambda item: hooks_installed()),
        pystray.MenuItem("Avvia con Windows", toggle_autostart, checked=lambda item: autostart_enabled()),
        pystray.MenuItem("Crea collegamento sul desktop", lambda: threading.Thread(
            target=lambda: (create_desktop_shortcut(), hub.icon.notify("Collegamento creato sul desktop.",
                                                                        "Desktop Companion")),
            daemon=True).start()),
        pystray.MenuItem("Aggiornamenti", pystray.Menu(
            pystray.MenuItem(lambda item: "Aggiorna ora: " + hub.updates_text() if hub.updates else "Nessun aggiornamento",
                             lambda: hub.apply_updates(), enabled=lambda item: bool(hub.updates)),
            pystray.MenuItem("Controlla su GitHub", lambda: threading.Thread(target=hub.check_updates, daemon=True).start()),
            pystray.MenuItem("Riscrivi firmware sui dispositivi", lambda: hub.update_firmware()),
            pystray.MenuItem("Apri la pagina GitHub", lambda: os.startfile(updater.REPO_URL)),
        )),
        pystray.MenuItem("Apri cartella", lambda: os.startfile(ROOT)),
        pystray.MenuItem("Apri registro", lambda: os.startfile(LOG_FILE)),
        pystray.Menu.SEPARATOR,
        pystray.MenuItem("Esci", lambda icon: icon.stop()),
    )


def main():
    handler = RotatingFileHandler(LOG_FILE, maxBytes=200_000, backupCount=1, encoding="utf-8")
    logging.basicConfig(level=logging.INFO, handlers=[handler],
                        format="%(asctime)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("127.0.0.1", UDP_PORT))
    except OSError:
        user32.MessageBoxW(0, "Desktop Companion è già in esecuzione:\ncerca l'icona vicino all'orologio.",
                           "Desktop Companion", 0x40)
        sys.exit(1)

    hub = Hub()
    hub.icon = pystray.Icon(APP_NAME, make_icon(ICON_COLORS["off"]), "Desktop Companion", build_menu(hub))
    threading.Thread(target=hub.udp_loop, args=(sock,), daemon=True).start()
    threading.Thread(target=hub.serial_loop, daemon=True).start()
    threading.Thread(target=hub.usage_loop, daemon=True).start()
    threading.Thread(target=hub.weather_loop, daemon=True).start()
    threading.Thread(target=hub.music_loop, daemon=True).start()
    threading.Thread(target=hub.update_loop, daemon=True).start()
    log.info("avviato")
    hub.icon.run()


if __name__ == "__main__":
    main()
