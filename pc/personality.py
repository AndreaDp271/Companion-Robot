# Desktop Companion - personalità: cosa dice il robot e il suo umore di fondo,
# in base a cosa stai facendo al PC. Le frasi sono qui sotto: aggiungine pure.
# Il display ha solo caratteri ASCII: niente lettere accentate (usa E' invece di È).
import random
import time

# chiave -> (emozione, frasi). Emozioni: happy, excited, love, surprised, sad, sleepy, bored
PHRASES = {
    "game:minecraft": ("excited", ["Scaviamo!", "Occhio ai creeper!", "Si mina!"]),
    "game:sot": ("excited", ["Arrr! Si salpa!", "Tesori in vista!", "Issate le vele!"]),
    "game:cities": ("excited", ["Buongiorno, sindaco!", "Costruiamo!", "Occhio al traffico!"]),
    "game:ets2": ("excited", ["Si parte! Brum!", "Cintura allacciata!", "Buon viaggio!"]),
    "game:isonzo": ("excited", ["Elmetto su!", "Giu' la testa!", "Avanti!"]),
    "game:tlou": ("surprised", ["Occhio ai clicker!", "Silenzio...", "Torcia pronta!"]),
    "game:": ("excited", ["Si gioca!", "Partita!", "Fammi vedere!"]),
    "music": ("love", ["Bella questa!", "Si balla!", "Alza il volume!", "Che ritmo!"]),
    "video": ("happy", ["Cosa guardiamo?", "Popcorn pronti!", "Bel video!"]),
    "code": ("happy", ["Si programma!", "Scriviamo codice!", "Niente bug oggi!"]),
    "osm:view": ("excited", ["Dove andiamo?", "Che bella mappa!", "Esploriamo!"]),
    "osm:edit": ("excited", ["Mappiamo!", "Aggiungi tutto!", "Un nodo alla volta!"]),
    "back": ("love", ["Bentornato!", "Eccoti!", "Mi eri mancato!"]),
    "bored": ("bored", ["Mi annoio...", "Ci sei?", "Uffa..."]),
    "morning": ("happy", ["Buongiorno!", "Buona giornata!"]),
    "lunch": ("happy", ["Buon pranzo!", "Pausa pranzo?"]),
    "late": ("sleepy", ["E' tardi... a nanna?", "Che sonno..."]),
    "break": ("sad", ["Fai una pausa!", "Sgranchisciti!", "Riposa gli occhi!"]),
    "code_hour": ("surprised", ["Un'ora di codice! Pausa?"]),
    "hot_cpu": ("sad", ["Che fatica!", "Uff, che caldo!"]),
    "hot_gpu": ("surprised", ["GPU bollente!", "Scotta!"]),
    "claude_5h": ("surprised", ["Token quasi finiti!", "Piano coi token!"]),
    "claude_week": ("surprised", ["Settimana quasi finita!"]),
    "weather:cold": ("sad", ["Brrr, che freddo!", "Copriti bene!"]),
    "weather:hot": ("sad", ["Che caldo!", "Bevi tanta acqua!"]),
    "weather:rain": ("sad", ["Piove! Ombrello!", "Che pioggia..."]),
    "weather:snow": ("excited", ["Nevica!", "Pupazzo di neve?"]),
    "weather:storm": ("surprised", ["Temporale!", "Tuoni! Aiuto!"]),
    "weather:sun": ("happy", ["Che bel sole!", "Giornata splendida!"]),
    "weather:moon": ("love", ["Che bella serata", "Guarda le stelle!"]),
    "weather:fog": ("bored", ["Che nebbia...", "Non si vede niente!"]),
    "weather:cloud": ("happy", ["Un po' nuvoloso oggi"]),
    "chat": ("happy", ["Tutto ok?", "Bevi un po' d'acqua!", "Ciao!", "Che bello stare qui",
                       "Ti voglio bene!", "Sei bravo!", "Sorridi!"]),
}

BORED_AFTER = 300      # s senza toccare mouse/tastiera: si annoia
BACK_AFTER = 600       # s di assenza oltre i quali ti dice "bentornato"
BREAK_AFTER = 90 * 60  # s di attività continua: ti consiglia una pausa
CODE_HOUR = 60 * 60    # s di VS Code (con pause brevi) per il messaggio "un'ora di codice"
CHAT_EVERY = (20 * 60, 40 * 60)  # intervallo delle chiacchiere a caso


class Personality:
    def __init__(self, chatter=True):
        self.chatter = chatter
        self.queue = []            # (emozione, frase) da mandare al robot
        self.said = {}             # chiave -> ultima volta che l'ha detta
        self.prev_scene = None
        self.prev_weather = None
        self.last_tick = time.time()
        self.active_since = time.time()
        self.idle_announced = False
        self.away_start = None     # quando hai smesso di usare il PC
        self.code_secs = 0.0
        self.away_from_code = 0.0
        self.hot_cpu_since = None
        self.next_chat = time.time() + random.randint(*CHAT_EVERY)

    def say(self, key, cooldown=0, fallback=None):
        now = time.time()
        if now - self.said.get(key, 0) < cooldown:
            return
        emo, phrases = PHRASES.get(key) or PHRASES[fallback]
        self.said[key] = now
        self.queue.append((emo, random.choice(phrases)))

    def pop_message(self):
        return self.queue.pop(0) if self.queue else None

    def tick(self, scene, detail, idle, fullscreen, cpu, gpu, usage, weather=None, music=False):
        """Chiamata ogni secondo. Restituisce l'umore di fondo: bored, sleepy o none."""
        now = time.time()
        dt, self.last_tick = now - self.last_tick, now
        hour = time.localtime().tm_hour + time.localtime().tm_min / 60
        # a schermo intero (video, giochi) o con la musica non ti stai annoiando
        away = idle > BORED_AFTER and not fullscreen and not music

        # nuova attività: una frase a tema (al massimo ogni 10 minuti per la stessa)
        if (scene, detail) != self.prev_scene and scene != "none" and not away:
            key = f"{scene}:{detail}" if scene in ("game", "osm") else scene
            self.say(key, cooldown=600, fallback="game:")
        self.prev_scene = (scene, detail)

        # lontano dal PC: si annoia; al ritorno ti saluta
        if away and not self.idle_announced:
            self.idle_announced = True
            self.say("bored")
        elif idle < 5 and self.idle_announced:
            self.idle_announced = False
            if self.away_start and now - self.away_start >= BACK_AFTER:
                self.say("back")
        if away:
            self.active_since = now
            self.away_start = self.away_start or now - idle
        else:
            self.away_start = None

        # tanta attività senza pause
        if now - self.active_since > BREAK_AFTER:
            self.say("break", cooldown=60 * 60)

        # un'ora di VS Code (le pause sotto i 10 minuti non azzerano il conteggio)
        if scene == "code" and not away:
            self.code_secs += dt
            self.away_from_code = 0
        else:
            self.away_from_code += dt
            if self.away_from_code > 600:
                self.code_secs = 0
        if self.code_secs >= CODE_HOUR:
            self.code_secs = 0
            self.say("code_hour")

        # momenti della giornata (solo se sei al PC)
        if idle < 60:
            day = time.strftime("%Y-%m-%d")
            if 6 <= hour < 11:  # una volta al giorno
                self.say("morning:" + day, cooldown=86400, fallback="morning")
            elif 12.5 <= hour < 14:
                self.say("lunch:" + day, cooldown=86400, fallback="lunch")
            elif 0.5 <= hour < 5:
                self.say("late", cooldown=60 * 60)

        # PC sotto sforzo
        if cpu >= 90:
            self.hot_cpu_since = self.hot_cpu_since or now
            if now - self.hot_cpu_since > 15:
                self.say("hot_cpu", cooldown=30 * 60)
        else:
            self.hot_cpu_since = None
        if gpu and gpu[1] >= 80:
            self.say("hot_gpu", cooldown=30 * 60)

        # consumi di Claude alti
        if usage.get("five_hour", (0,))[0] >= 80:
            self.say("claude_5h", cooldown=60 * 60)
        if usage.get("seven_day", (0,))[0] >= 90:
            self.say("claude_week", cooldown=6 * 60 * 60)

        # il tempo è cambiato (o è la prima lettura): lo commenta, se sei al PC
        if weather and weather != self.prev_weather and idle < 60:
            self.prev_weather = weather
            self.say("weather:" + weather, cooldown=3 * 60 * 60)

        # chiacchiere ogni tanto, quando sei al PC e non stai giocando
        if self.chatter and now > self.next_chat:
            self.next_chat = now + random.randint(*CHAT_EVERY)
            if idle < 60 and scene in ("none", "code"):
                self.say("chat")

        if away:
            return "bored"
        if 0.5 <= hour < 5:
            return "sleepy"
        return "none"
