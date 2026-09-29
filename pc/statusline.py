# Barra di stato di Claude Code: mostra i consumi (5 ore / settimana) e li manda al
# companion hub. Claude Code passa i dati della sessione in JSON su stdin.
import json
import socket
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

try:
    data = json.loads(sys.stdin.buffer.read().decode("utf-8") or "{}")
except ValueError:
    data = {}

limits = data.get("rate_limits") or {}


def window(name):
    w = limits.get(name) or {}
    return w.get("used_percentage"), w.get("resets_at")


def num(v):
    return "-" if v is None else str(round(v))


def until(ts):
    mins = max(0, int((ts - time.time()) // 60))
    return f"{mins // 60}h{mins % 60:02d}" if mins >= 60 else f"{mins}min"


p5, r5 = window("five_hour")
p7, r7 = window("seven_day")

try:
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(
        f"usage {num(p5)} {num(r5)} {num(p7)} {num(r7)}".encode(), ("127.0.0.1", 47800))
except OSError:
    pass

parts = [(data.get("model") or {}).get("display_name", "Claude")]
if p5 is not None:
    parts.append(f"5h {p5:.0f}%" + (f" (reset tra {until(r5)})" if r5 else ""))
if p7 is not None:
    parts.append(f"settimana {p7:.0f}%")
print(" · ".join(parts))
