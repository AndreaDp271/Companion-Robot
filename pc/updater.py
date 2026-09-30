# Desktop Companion - aggiornamenti da GitHub (app, firmware del robot RP2040 e del Nesso N1).
# La repo contiene version.json con le versioni pubblicate: se sono più nuove di quelle
# installate, scarica lo zip della repo e aggiorna ciò che serve.
import json
import shutil
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPO = "AndreaDp271/Companion-Robot"
BRANCH = "main"
VERSION_URL = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}/version.json"
VERSION_API_URL = f"https://api.github.com/repos/{REPO}/contents/version.json?ref={BRANCH}"
ZIP_URL = f"https://github.com/{REPO}/archive/refs/heads/{BRANCH}.zip"
REPO_URL = f"https://github.com/{REPO}"

# file dell'utente che un aggiornamento non deve mai toccare
KEEP = {"config.json", "companion.log", "companion.log.1", "secrets.h"}


def parse_version(v):
    try:
        return tuple(int(x) for x in str(v).split("."))
    except ValueError:
        return (0,)


def local_versions(root):
    try:
        return json.loads((Path(root) / "version.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def remote_versions():
    try:
        req = urllib.request.Request(VERSION_URL, headers={"User-Agent": "desktop-companion", "Cache-Control": "no-cache"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.load(r)
    except OSError:
        # piano B: l'API di GitHub (raw.githubusercontent.com può avere in cache un vecchio 404)
        req = urllib.request.Request(VERSION_API_URL, headers={"User-Agent": "desktop-companion",
                                                               "Accept": "application/vnd.github.raw"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.load(r)


def pending(root, installed_fw):
    """Cosa c'è da aggiornare: {"app": (vecchia, nuova), "rp2040": ..., "nesso": ...}.
    installed_fw: versioni del firmware già scritte sui dispositivi (salvate in config)."""
    local, remote = local_versions(root), remote_versions()
    out = {}
    if parse_version(remote.get("app", 0)) > parse_version(local.get("app", 0)):
        out["app"] = (local.get("app", "?"), remote["app"])
    for dev in ("rp2040", "nesso"):
        have = installed_fw.get(dev) or local.get(dev, "0")
        if parse_version(remote.get(dev, 0)) > parse_version(have):
            out[dev] = (have, remote[dev])
    return out


def download(dest_dir):
    """Scarica e scompatta la repo; restituisce la cartella con i file."""
    zip_path = Path(dest_dir) / "companion.zip"
    req = urllib.request.Request(ZIP_URL, headers={"User-Agent": "desktop-companion"})
    with urllib.request.urlopen(req, timeout=60) as r, open(zip_path, "wb") as f:
        shutil.copyfileobj(r, f)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(dest_dir)
    return next(p for p in Path(dest_dir).iterdir() if p.is_dir())  # Companion-Robot-main/


def install_app(src, root):
    """Copia la nuova versione dell'app sopra quella installata, lasciando i file dell'utente."""
    for item in Path(src).rglob("*"):
        rel = item.relative_to(src)
        if item.name in KEEP or ".git" in rel.parts:
            continue
        target = Path(root) / rel
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, target)


def new_temp_dir():
    return tempfile.mkdtemp(prefix="companion_update_")
