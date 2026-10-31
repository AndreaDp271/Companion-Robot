# Desktop Companion - Adafruit MacroPad: cosa fanno i tasti e l'encoder.
# Il macropad manda "!k <n>" (tasto premuto), "!h <n>" (tenuto premuto), "!e spk|mic <scatti>"
# (encoder): qui diventano azioni sul PC. L'audio passa da AudioTray (UDP 127.0.0.1:47801),
# così il suo OSD e le sue impostazioni restano le stesse del pannello.
# I tasti si cambiano in config.json: "macropad": {"keys": [...]}, stesso formato di KEYS.
import asyncio
import ctypes
import logging
import os
import socket
import subprocess
import time
from ctypes import wintypes
from datetime import datetime, timezone
from pathlib import Path

import psutil

try:  # controllo multimediale di Windows (lo stesso che companion.py usa per leggere la musica)
    from winrt.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus)
except ImportError:
    MediaManager = None

log = logging.getLogger("companion")

AUDIOTRAY_PORT = 47801
AUDIOTRAY_EXE = Path.home() / "AudioTray" / "AudioTray.exe"
STEP = 2  # % di volume per scatto dell'encoder (lo stesso del firmware)
LOCAL = os.environ.get("LOCALAPPDATA", "")

# Tasti da sinistra a destra, dall'alto in basso (0-2 è la fila sotto lo schermo).
# Casse <-> cuffie è sulla pressione della manopola.
#   color: luce fissa del tasto (i tasti di audio e microfono diventano rossi quando sono muti)
#   role:  spk e mic seguono il muto, companion cambia pagina sullo schermo (tenuto: schermo spento)
#   press / hold: "audio <comando AudioTray>", "media play|next|prev", "app" (porta in primo
#          piano exe, solo le finestre con title nel titolo se c'è, se non c'è avvia run; se è già davanti passa alla finestra dopo o la
#          riduce a icona), "run" (avvia sempre run), "open <file o indirizzo>"
KEYS = [
    {"name": "Audio", "color": "ffffff", "role": "spk", "press": "audio spk toggle", "hold": "audio panel"},
    {"name": "Microfono", "color": "00ff00", "role": "mic", "press": "audio mic toggle", "hold": "audio panel"},
    {"name": "CastelSafe", "color": "ffd000", "role": "app", "press": "app",  # app web di Chrome
     "exe": "chrome.exe", "title": "castelsafe",
     "run": r"%USERPROFILE%\Desktop\CastelSafe - ARPAV e Protezione civile.lnk"},
    {"name": "Indietro", "color": "ff00b0", "role": "media", "press": "media prev"},
    {"name": "Play/Pausa", "color": "ff00b0", "role": "media", "press": "media play"},
    {"name": "Avanti", "color": "ff00b0", "role": "media", "press": "media next"},
    {"name": "VS Code", "color": "0040ff", "role": "app", "press": "app", "hold": "run",
     "exe": "code.exe", "run": rf"{LOCAL}\Programs\Microsoft VS Code\Code.exe"},
    {"name": "Brave", "color": "ff3000", "role": "app", "press": "app", "hold": "run",
     "exe": "brave.exe", "run": r"C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe"},
    {"name": "Esplora file", "color": "00c0a0", "role": "app", "press": "app", "hold": "run",
     "exe": "explorer.exe", "run": "explorer.exe"},
    {"name": "Discord", "color": "7020ff", "role": "app", "press": "app", "hold": "run",
     "exe": "discord.exe", "run": rf"{LOCAL}\Discord\Update.exe --processStart Discord.exe"},
    {"name": "Telegram", "color": "00a0ff", "role": "app", "press": "app", "hold": "run",
     "exe": "telegram.exe", "run": r"D:\Telegram Desktop\Telegram.exe"},
    {"name": "Companion", "color": "80ff00", "role": "companion"},  # gestito dal macropad
]
MEDIA_VK = {"play": 0xB3, "next": 0xB0, "prev": 0xB1}
SEEK_S = 10  # secondi di salto avanti/indietro quando non c'è un brano dopo/prima

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
_EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


# ---------------------------------------------------------------- AudioTray

def audiotray(cmd):
    """Manda un comando ad AudioTray. Restituisce (vol, muto, mic, muto) come stringhe
    ("-" se il dispositivo manca) e il nome dell'uscita, o None se non risponde."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(0.5)
        try:
            s.sendto(cmd.encode(), ("127.0.0.1", AUDIOTRAY_PORT))
            reply = s.recv(512).decode("utf-8", "ignore")
        except OSError:
            return None
    levels, _, name = reply.partition("|")
    v = levels.split()
    return (tuple(v), name) if len(v) == 4 else None


def audiotray_running():
    return any((p.info["name"] or "").lower() == "audiotray.exe" for p in psutil.process_iter(["name"]))


def short_output(name):
    """Nome breve dell'uscita per lo schermo: "Cuffie", "Casse" o il nome senza la scheda."""
    n = name.lower()
    if any(w in n for w in ("cuffie", "headphone", "headset", "auricolar", "buds", "airpods")):
        return "Cuffie"
    if any(w in n for w in ("casse", "altoparlant", "speaker")):
        return "Casse"
    return name.split(" (")[0] or "?"


# ---------------------------------------------------------------- finestre e tasti

def _cloaked(hwnd):
    """Finestre "nascoste" da Windows (app UWP sospese, altri desktop virtuali)."""
    v = ctypes.c_int(0)
    ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(v), ctypes.sizeof(v))
    return v.value != 0


def app_windows(exe, title=""):
    """Finestre principali dell'app, dalla più in primo piano alla più indietro."""
    pids = {p.pid for p in psutil.process_iter(["name"]) if (p.info["name"] or "").lower() == exe}
    found = []
    cls = ctypes.create_unicode_buffer(64)

    def cb(hwnd, _):
        if (not user32.IsWindowVisible(hwnd) or user32.GetWindow(hwnd, 4)  # 4 = GW_OWNER: finestre figlie
                or not user32.GetWindowTextLengthW(hwnd) or _cloaked(hwnd)):
            return True
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids:
            user32.GetClassNameW(hwnd, cls, 64)
            # explorer.exe è anche barra delle applicazioni e desktop: contano solo le cartelle
            if exe == "explorer.exe" and cls.value != "CabinetWClass":
                return True
            if title:  # es. un'app web di Chrome: stesso exe del browser, la distingue il titolo
                buf = ctypes.create_unicode_buffer(256)
                user32.GetWindowTextW(hwnd, buf, 256)
                if title not in buf.value.lower():
                    return True
            found.append(hwnd)
        return True

    user32.EnumWindows(_EnumProc(cb), 0)
    return found


def activate(hwnd):
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    # Windows dà il primo piano solo a chi ha ricevuto l'ultimo input: mi aggancio al thread
    # della finestra attuale per il tempo dello scambio
    me = kernel32.GetCurrentThreadId()
    fg = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
    user32.AttachThreadInput(me, fg, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(me, fg, False)


def launch(run):
    run = os.path.expandvars(run)
    if run.lower().endswith((".lnk", ".url")) or "://" in run or run.startswith("ms-settings:"):
        os.startfile(run)
    else:
        subprocess.Popen(run, creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
                         close_fds=True)


def focus_or_launch(exe, run, title=""):
    wins = app_windows(exe, title)
    if not wins:
        if run:
            launch(run)
        return
    fg = user32.GetForegroundWindow()
    if fg == wins[0]:  # è già davanti: passa alla prossima finestra, o riducila a icona
        if len(wins) > 1:
            activate(wins[-1])
        else:
            user32.ShowWindow(fg, 6)  # SW_MINIMIZE
    else:
        activate(wins[0])


async def _media_session(name):
    mgr = await MediaManager.request_async()
    sessions = list(mgr.get_sessions())
    # quella che sta suonando, se no quella che Windows considera attuale
    s = next((x for x in sessions if x.get_playback_info().playback_status == PlaybackStatus.PLAYING),
             mgr.get_current_session())
    if not s:
        return False
    if name == "play":
        return await s.try_toggle_play_pause_async()
    info = s.get_playback_info()
    if info.controls.is_next_enabled if name == "next" else info.controls.is_previous_enabled:
        return await (s.try_skip_next_async() if name == "next" else s.try_skip_previous_async())
    if info.controls.is_playback_position_enabled:
        # niente brano dopo/prima (es. un video singolo su YouTube): salta di 10 secondi
        tl = s.get_timeline_properties()
        pos = tl.position.total_seconds()
        if info.playback_status == PlaybackStatus.PLAYING:
            pos += (datetime.now(timezone.utc) - tl.last_updated_time).total_seconds()
        end = (tl.end_time - tl.start_time).total_seconds()
        pos = max(0.0, min(end, pos + (SEEK_S if name == "next" else -SEEK_S)))
        return await s.try_change_playback_position_async(int(pos * 10_000_000))  # in unità da 100 ns
    return False


def media_key(name):
    """Play/pausa, avanti, indietro: direttamente alla sessione multimediale di Windows (Spotify,
    YouTube nel browser...); il tasto multimediale simulato solo se quella non risponde."""
    if MediaManager:
        try:
            if asyncio.run(_media_session(name)):
                return
            log.info("macropad: %s non accettato dal player, provo col tasto multimediale", name)
        except Exception as e:  # errori WinRT
            log.info("macropad: controllo multimediale non riuscito: %s", e)
    vk = MEDIA_VK[name]
    user32.keybd_event(vk, 0, 1, 0)  # KEYEVENTF_EXTENDEDKEY
    user32.keybd_event(vk, 0, 3, 0)  # ... | KEYEVENTF_KEYUP


# ---------------------------------------------------------------- macropad

class Macropad:
    def __init__(self, cfg):
        self.keys = cfg.get("macropad", {}).get("keys") or KEYS
        self.audio = None  # ultima risposta di AudioTray (vedi audiotray)
        self.out_name = None  # uscita appena scelta col tasto, da mostrare una volta
        self.keys_sent = 0.0
        self.started_audiotray = 0.0

    def poll_audio(self):
        self.audio = audiotray("state")
        if self.audio is None and time.time() - self.started_audiotray > 60 and AUDIOTRAY_EXE.exists() \
                and not audiotray_running():
            self.started_audiotray = time.time()
            log.info("AudioTray non risponde: lo avvio")
            launch(str(AUDIOTRAY_EXE))

    def lines(self, ascii_text, fresh=False):
        """Righe in più per il macropad: K (audio), D (uscita attiva), Y (tasti, ogni 10 s o appena
        collegato), O (uscita appena cambiata)."""
        out = "K - 0 - 0\n" if not self.audio else "K %s %s %s %s\n" % self.audio[0]
        if self.audio:
            out += f"D {ascii_text(short_output(self.audio[1]), 12)}\n"
        if fresh or time.time() - self.keys_sent > 10:
            self.keys_sent = time.time()
            for i, k in enumerate(self.keys[:12]):
                out += f"Y {i} {k.get('color', 'ffffff')} {k.get('role', 'app')} {ascii_text(k['name'], 20)}\n"
        if self.out_name:
            out += f"O {ascii_text(self.out_name, 40)}\n"
            self.out_name = None
        return out

    def command(self, cmd):
        """Comando dal macropad. True se l'audio è cambiato (va rimandato subito lo stato)."""
        p = cmd.split()
        if p[0] == "!e" and len(p) == 3 and p[1] in ("spk", "mic"):
            return self.audio_cmd(f"{p[1]} {int(p[2]) * STEP:+d}")
        if p[0] in ("!k", "!h") and len(p) == 2:
            if p[1] == "enc":  # pressione dell'encoder: casse <-> cuffie, tenuto: pannello di AudioTray
                return self.audio_cmd("out next" if p[0] == "!k" else "panel")
            if p[1].isdigit() and int(p[1]) < len(self.keys):
                key = self.keys[int(p[1])]
                return self.run(key.get("press" if p[0] == "!k" else "hold"), key)
        return False

    def run(self, action, key):
        if not action:
            return False
        what, _, arg = action.partition(" ")
        log.info("macropad: %s -> %s", key["name"], action)
        try:
            if what == "audio":
                return self.audio_cmd(arg)
            if what == "media":
                media_key(arg)
            elif what == "app":
                focus_or_launch(key.get("exe", "").lower(), key.get("run"), key.get("title", "").lower())
            elif what == "run":
                launch(key["run"])
            elif what == "open":
                os.startfile(arg)
        except OSError as e:
            log.info("macropad: %s non riuscito: %s", action, e)
        return False

    def audio_cmd(self, cmd):
        before = self.audio[1] if self.audio else None
        self.audio = audiotray(cmd)
        if cmd == "out next" and self.audio and self.audio[1] != before:
            self.out_name = self.audio[1]
        return True
