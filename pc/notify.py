# Manda un evento al companion hub. Uso: python notify.py work|ask|done|none
import socket
import sys

try:
    event = sys.argv[1] if len(sys.argv) > 1 else "none"
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(event.encode(), ("127.0.0.1", 47800))
except OSError:
    pass
