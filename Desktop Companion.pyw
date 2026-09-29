# Doppio clic per avviare Desktop Companion (senza finestra del terminale).
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "pc"))

import companion

companion.main()
