/*
 * ================================================================
 *  Pagina COMPANION - Desktop Companion sul Nesso N1 (ORIZZONTALE)
 * ================================================================
 *  Gli stessi occhioni del robot RP2040, a colori. Solo questa pagina ruota lo schermo
 *  in orizzontale (240x135); le altre pagine della Suite restano verticali.
 *
 *  L'app "Desktop Companion" sul PC manda via USB (seriale 115200) righe di testo,
 *  le stesse del robot RP2040:
 *    S <cpu> <ram> <HH:MM>        stato del PC
 *    A <work|ask|done|none>       Claude Code
 *    P <scena> [dettaglio]        code | game <gioco> | osm view|edit | music | video | none
 *    U <5h%> <sett%> <r5> <r7>    consumi di Claude ("-" se mancano)
 *    V <gpu%> <temp> <vram%>      GPU
 *    E <bored|sleepy|none>        umore di fondo
 *    M <emozione> <frase>         frase nella nuvoletta
 *    W <meteo> <temp>             meteo (sun, rain, snow, ...)
 *    H <1|0|->  N <titolo>  B <pos> <durata>   musica/video in riproduzione
 *  Il Nesso risponde con:
 *    !update                      (KEY2 tenuto premuto) aggiorna app e firmware dalla repo GitHub
 *
 *  La faccia usa tutto lo schermo; consumi, CPU/RAM/GPU e meteo sono nelle viste di KEY2.
 *  KEY2 breve = vista successiva (occhi -> consumi Claude -> orologio), come il tasto del RP2040.
 *  KEY2 lungo = aggiornamento dal PC; senza app sul PC apre la pagina GitHub con le istruzioni
 *  via tastiera Bluetooth (o mostra il QR).
 */

#define COMP_REPO_URL   "https://github.com/AndreaDp271/Companion-Robot"
#define COMP_TIMEOUT_MS 8000    // senza dati dal PC da tanto -> schermata di installazione
#define COMP_VIEW_MS    20000   // dopo quanto si torna agli occhi
#define COMP_LONG_MS    1000    // pressione lunga di KEY2

const int CW = 240, CH = 135;   // schermo in orizzontale
const int CTOP = 14;            // barra in alto
const int FACE_W = CW;          // occhi e scena a tutta larghezza (le info sono nelle viste di KEY2)
const int XO = 45;              // spostamento per centrare le scenette disegnate larghe 150

M5Canvas cc(&M5.Display);       // sprite orizzontale della pagina (creato al primo uso)
bool ccReady = false;

// ---- stato ricevuto dal PC ----
struct CompState {
  int cpu = 0, ram = 0; String clock = "--:--";
  uint32_t last = 0; bool everData = false;
  String act = "none", scene = "none", detail = "", emo = "none";
  String u5 = "-", u7 = "-", r5 = "-", r7 = "-";
  int gpu = -1, gtemp = -1;
  String msgEmo, msgText; uint32_t msgUntil = 0;
  String wx = ""; int wxTemp = 0;
  int media = -1;              // -1 niente, 0 video, 1 musica
  String song; int mPos = 0, mDur = 0; uint32_t mAt = 0;
} cs;

String compLine;
String compPrevAct = "none";

// animazione meteo: ogni 4-7 minuti per 8 secondi, se il companion non sta facendo altro
#define WX_ANIM_MS 8000
struct { String kind; uint32_t until = 0, next = 0; bool forced = false; } wxa;
String compFest;                                // festa di oggi (dal PC), "" se non è festa
int compView = 0; uint32_t compViewUntil = 0;   // 0 occhi, 1 consumi Claude, 2 orologio
bool compShowQr = false;                        // QR di installazione (anche col PC collegato)
uint32_t compUpdAt = 0;                         // quando è stato chiesto l'aggiornamento
struct { bool down = false; uint32_t since = 0; bool longDone = false; } ck;

bool companionPcConnected(){ return cs.everData && millis() - cs.last < COMP_TIMEOUT_MS; }
void renderCompanion();

// ---- installazione dell'app via tastiera Bluetooth ----
// L'ESP32-C6 non ha USB OTG (solo USB seriale/JTAG), quindi non può fare da tastiera USB:
// usa la tastiera Bluetooth (report 2 dello stesso dispositivo HID del mouse). Con il Nesso
// associato a Windows, apre Esegui (Win+R) e scrive l'indirizzo della repo: si apre il browser
// con le istruzioni. Niente comandi PowerShell: gli antivirus li bloccherebbero come sospetti.
// Ogni carattere è scritto con Alt + codice sul tastierino numerico: funziona con qualsiasi layout.
#define COMP_INSTALL_CMD COMP_REPO_URL
int compTyping = -1;          // avanzamento della scrittura (%), -1 = non sta scrivendo
uint32_t compInstallAt = 0;   // quando ha finito di scrivere il comando
const uint8_t KEYPAD[10] = {0x62, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61};   // 0..9 del tastierino

bool compKeyboardReady(){ return mouseInited && bleConnected && mInputKb; }
void kbReport(uint8_t mod, uint8_t key){
  uint8_t r[8] = {mod, 0, key, 0, 0, 0, 0, 0};
  mInputKb->setValue(r, sizeof(r)); mInputKb->notify(); delay(12);
}
void kbTap(uint8_t mod, uint8_t key){ kbReport(mod, key); kbReport(mod, 0); }
void kbAltChar(char c){        // Alt tenuto + codice decimale sul tastierino, poi rilascio Alt
  char d[4]; snprintf(d, sizeof(d), "%d", (uint8_t)c);
  kbReport(0x04, 0);
  for(char* p = d; *p; p++){ kbReport(0x04, KEYPAD[*p - '0']); kbReport(0x04, 0); }
  kbReport(0, 0);
}
bool compBleInstall(){
  if(!compKeyboardReady()) return false;
  if(!kbLedKnown || !(kbLeds & 1)){          // i codici Alt vogliono Bloc Num acceso
    kbTap(0, 0x53); delay(250);
    if(kbLedKnown && !(kbLeds & 1)){ kbTap(0, 0x53); delay(250); }
  }
  kbTap(0, 0x29); delay(150);                 // Esc: chiude eventuali menu aperti
  kbTap(0x08, 0x15); delay(900);              // Win+R: finestra Esegui
  const char* cmd = COMP_INSTALL_CMD; int n = strlen(cmd);
  for(int i = 0; i < n; i++){
    kbAltChar(cmd[i]);
    if(i % 6 == 0){ compTyping = i * 100 / n; renderCompanion(); }
  }
  kbTap(0, 0x28);                             // Invio
  compTyping = -1; compInstallAt = millis();
  return true;
}

// KEY2 sulla pagina companion (stato già filtrato dal debounce della Suite)
void companionKey(bool down){
  uint32_t now = millis();
  if(down && !ck.down){ ck.down = true; ck.since = now; ck.longDone = false; }
  else if(down && !ck.longDone && now - ck.since >= COMP_LONG_MS){   // lungo
    ck.longDone = true;
    if(companionPcConnected()){ Serial.println("!update"); compUpdAt = now; compShowQr = false; }   // app c'è: aggiorna
    else if(compKeyboardReady()){ compShowQr = false; compBleInstall(); }                         // app non c'è: installala
    else compShowQr = true;                                                                       // spiega come fare
  }
  else if(!down && ck.down){
    ck.down = false;
    if(!ck.longDone){                                                 // breve
      if(compShowQr) compShowQr = false;
      else { compView = (compView + 1) % 3; compViewUntil = now + COMP_VIEW_MS; }
    }
  }
}

// ---- colori ----
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b){ return cc.color565(r, g, b); }
uint16_t hueColor(int h){      // arcobaleno: h 0..359
  h = ((h % 360) + 360) % 360; int x = (h % 60) * 255 / 60;
  switch(h / 60){
    case 0: return rgb(255, x, 0);       case 1: return rgb(255 - x, 255, 0);
    case 2: return rgb(0, 255, x);       case 3: return rgb(0, 255 - x, 255);
    case 4: return rgb(x, 0, 255);       default: return rgb(255, 0, 255 - x);
  }
}
uint16_t moodColor(const String& m){
  if(m == "happy" || m == "done") return rgb(80, 230, 120);
  if(m == "stress") return rgb(255, 70, 60);
  if(m == "work")   return rgb(180, 110, 255);
  if(m == "ask")    return rgb(255, 150, 40);
  if(m == "code")   return rgb(70, 150, 255);
  if(m == "bored")  return rgb(140, 140, 150);
  if(m == "sleepy") return rgb(70, 90, 190);
  if(m == "love")   return rgb(255, 90, 170);
  if(m == "excited")return rgb(255, 210, 60);
  if(m == "surprised") return TFT_WHITE;
  if(m == "sad")    return rgb(110, 170, 255);
  if(m == "osm")    return rgb(126, 188, 111);
  if(m == "video")  return rgb(150, 200, 255);
  return rgb(0, 200, 255);   // normale: azzurro
}
uint16_t gameColor(const String& g){
  if(g == "minecraft") return rgb(90, 200, 70);
  if(g == "sot")    return rgb(40, 190, 190);
  if(g == "cities") return rgb(250, 200, 40);
  if(g == "ets2")   return rgb(255, 140, 30);
  if(g == "isonzo") return rgb(170, 160, 90);
  if(g == "tlou")   return rgb(120, 170, 90);
  return rgb(255, 80, 80);
}
String gameName(const String& g){
  if(g == "sot") return "Sea of Thieves";   if(g == "ets2") return "Euro Truck";
  if(g == "tlou") return "The Last of Us";  if(g == "minecraft") return "Minecraft";
  if(g == "cities") return "Cities";        if(g == "isonzo") return "Isonzo";
  return "Si gioca!";
}
uint16_t levelColor(int p){ return p < 50 ? rgb(80, 220, 110) : (p < 80 ? rgb(250, 200, 40) : rgb(255, 70, 60)); }

// ---- ricezione dal PC ----
void compParse(const String& line){
  if(line.length() < 3 || line[1] != ' ') return;
  char k = line[0];
  String rest = line.substring(2);
  if(k == 'M'){   // M <emozione> <frase>
    int sp = rest.indexOf(' ');
    if(sp > 0){ cs.msgEmo = rest.substring(0, sp); cs.msgText = rest.substring(sp + 1); cs.msgUntil = millis() + 4500; compUpdAt = 0; }
    return;
  }
  if(k == 'N'){ cs.song = rest; return; }
  String p[5]; int n = 0, from = 0;   // gli altri comandi: parole separate da spazi
  while(n < 5 && from <= (int)rest.length()){
    int sp = rest.indexOf(' ', from); if(sp < 0) sp = rest.length();
    if(sp > from) p[n++] = rest.substring(from, sp);
    from = sp + 1;
  }
  switch(k){
    case 'S': if(n == 3){ cs.cpu = p[0].toInt(); cs.ram = p[1].toInt(); cs.clock = p[2]; cs.last = millis(); cs.everData = true; } break;
    case 'A': if(n == 1) cs.act = p[0]; break;
    case 'P': if(n >= 1){ cs.scene = p[0]; cs.detail = n >= 2 ? p[1] : ""; } break;
    case 'E': if(n == 1) cs.emo = p[0]; break;
    case 'U': if(n == 4){ cs.u5 = p[0]; cs.u7 = p[1]; cs.r5 = p[2]; cs.r7 = p[3]; } break;
    case 'V': if(n == 3){ cs.gpu = p[0] == "-" ? -1 : p[0].toInt(); cs.gtemp = p[1] == "-" ? -1 : p[1].toInt(); } break;
    case 'W': if(n == 2){ cs.wx = p[0] == "-" ? "" : p[0]; cs.wxTemp = p[1].toInt(); } break;
    case 'F': if(n == 1) compFest = p[0] == "-" ? "" : p[0]; break;
    case 'T': if(n == 1){ wxa.kind = p[0]; wxa.until = millis() + WX_ANIM_MS; wxa.forced = true; } break;   // prova dal menu
    case 'H': if(n == 1){ cs.media = p[0] == "1" ? 1 : (p[0] == "0" ? 0 : -1); if(cs.media < 0){ cs.song = ""; cs.mDur = 0; } } break;
    case 'B': if(n == 2){ cs.mPos = p[0].toInt(); cs.mDur = p[1].toInt(); cs.mAt = millis(); } break;
  }
}

// ---- rotazione automatica (accelerometro) ----
// COMPANION è in orizzontale (rotazione 1 o 3), le altre pagine in verticale (0 o 2): lo schermo
// si capovolge quando capovolgi il Nesso. Misurato: tenuto in orizzontale e dritto, la gravità
// è tutta su ay (+1) -> ay = asse corto dello schermo, ax = asse lungo.
// Il verso degli assi si regola dal PC senza ricompilare: "#rot l -1" / "#rot p -1" (salvato).
// Le pagine che usano l'inclinazione (BOLLA, GIOCO, MOUSE) restano ferme.
Preferences rotPrefs;
int8_t rotSignL = 1, rotSignP = 1;   // verso degli assi: orizzontale (COMPANION) e verticale
int curRot = -1;
float rotAx = 0, rotAy = 0;          // accelerazione filtrata
uint32_t rotLast = 0;

void rotLoad(){
  rotPrefs.begin("rot", true);
  rotSignL = rotPrefs.getChar("l", 1); rotSignP = rotPrefs.getChar("p", 1);
  rotPrefs.end();
}
void rotSave(){
  rotPrefs.begin("rot", false);
  rotPrefs.putChar("l", rotSignL); rotPrefs.putChar("p", rotSignP);
  rotPrefs.end();
}
void autoRotate(bool force){
  bool fixed = page == PG_LEVEL || page == PG_GAME || page == PG_MOUSE;
  if(!force && (fixed || millis() - rotLast < 200)) return;   // ~5 letture al secondo
  rotLast = millis();
  float ax = 0, ay = 0, az = 0;
  if(!fixed){ M5.Imu.update(); M5.Imu.getAccelData(&ax, &ay, &az); }
  rotAx = rotAx * 0.6f + ax * 0.4f; rotAy = rotAy * 0.6f + ay * 0.4f;
  bool land = page == PG_COMP;
  int base = land ? 1 : 0, flip = land ? 3 : 2;
  float g = land ? rotAy * rotSignL : rotAx * rotSignP;   // orizzontale: asse corto (ay); verticale: asse lungo (ax)
  int want = (curRot == base || curRot == flip) ? curRot : base;   // tra le due soglie resta com'è
  if(!fixed){ if(g > 0.55f) want = base; else if(g < -0.55f) want = flip; }
  else want = base;
  if(want != curRot || force){
    bool changed = want != curRot;
    curRot = want;
    M5.Display.setRotation(curRot);
    if(changed && !force) render();
  }
}

// "#shot": manda l'immagine della pagina (RGB565, 240x135) per controllare la grafica dal PC
void compScreenshot(){
  if(!ccReady) return;
  Serial.printf("SHOT %d %d\n", CW, CH);
  Serial.write((const uint8_t*)cc.getBuffer(), CW * CH * 2);
  Serial.flush();
}

void companionPollSerial(){
  while(Serial.available()){
    char c = Serial.read();
    if(c == '\n'){
      compLine.trim();
      if(compLine == "#shot") compScreenshot();
      else if(compLine == "#imu"){   // per tarare la rotazione: accelerazione e rotazione attuali
        float ax = 0, ay = 0, az = 0; M5.Imu.update(); M5.Imu.getAccelData(&ax, &ay, &az);
        Serial.printf("IMU ax=%.2f ay=%.2f az=%.2f rot=%d segni l=%d p=%d\n", ax, ay, az, curRot, rotSignL, rotSignP);
      }
      else if(compLine.startsWith("#rot ") && compLine.length() >= 8){   // "#rot l -1" / "#rot p 1"
        int8_t v = compLine.substring(7).toInt() < 0 ? -1 : 1;
        if(compLine[5] == 'l') rotSignL = v; else rotSignP = v;
        rotSave(); autoRotate(true);
        Serial.printf("ROT segni l=%d p=%d\n", rotSignL, rotSignP);
      }
      else compParse(compLine);
      compLine = "";
    }
    else if(compLine.length() < 120) compLine += c;
  }
  // Claude ti cerca: se sei sul companion con lo schermo spento, lo riaccende
  if(cs.act != compPrevAct){
    if(cs.act == "ask" && page == PG_COMP && !screenOn) wakeScreen();
    compPrevAct = cs.act;
  }
}

// ---- umore (stessa logica del robot RP2040) ----
String compMood(){
  if(cs.act == "work" || cs.act == "ask" || cs.act == "done") return cs.act;
  if(cs.emo == "bored") return "bored";
  if(cs.scene == "code" || cs.scene == "game" || cs.scene == "osm" || cs.scene == "music" || cs.scene == "video") return cs.scene;
  if(cs.emo == "sleepy") return "sleepy";
  if(cs.cpu >= 80 || cs.gpu >= 90 || cs.gtemp >= 80) return "stress";
  if(cs.cpu <= 15) return "happy";
  return "normal";
}

// ---- animazione: battito di palpebre e sguardo ----
struct { uint32_t blinkAt = 0; int blinkI = -1; uint32_t lookAt = 0; float lx = 0, ly = 0; int tx = 0, ty = 0; } fx;
const float BLINK_K[] = {0.6f, 0.2f, 0.05f, 0.2f, 0.6f};

float compBlink(uint32_t now){
  if(fx.blinkI < 0 && (int32_t)(now - fx.blinkAt) >= 0) fx.blinkI = 0;
  if(fx.blinkI < 0) return 1.0f;
  float k = BLINK_K[fx.blinkI++];
  if(fx.blinkI >= 5){ fx.blinkI = -1; fx.blinkAt = now + random(1500, 5000); }
  return k;
}
void compLook(uint32_t now, const String& m){
  if((int32_t)(now - fx.lookAt) >= 0){
    if(m == "work"){ fx.tx = 5 + random(-1, 2); fx.ty = -3; fx.lookAt = now + random(150, 400); }
    else if(m == "ask"){ fx.tx = 0; fx.ty = 0; fx.lookAt = now + 3000; }
    else if(m == "game"){ fx.tx = random(-6, 7); fx.ty = random(-2, 3); fx.lookAt = now + random(150, 500); }
    else if(m == "bored" || m == "sleepy"){ fx.tx = fx.tx <= 0 ? 8 : -8; fx.ty = 4; fx.lookAt = now + random(3000, 5000); }
    else { fx.tx = random(-9, 10); fx.ty = random(-5, 6); fx.lookAt = now + random(1200, 4000); }
  }
  float sp = m == "game" ? 0.5f : 0.2f;
  fx.lx += (fx.tx - fx.lx) * sp; fx.ly += (fx.ty - fx.ly) * sp;
}

// ---- goccia di sudore (la usa la mascotte di Claude) ----
void compSweat(int x, int y){
  uint16_t c = rgb(120, 200, 255);
  cc.fillCircle(x, y, 3, c); cc.fillTriangle(x - 3, y - 1, x + 3, y - 1, x, y - 8, c);
}
void compWxIcon(const String& k, int cx, int cy, uint32_t now){   // meteo
  if(k == "sun"){ cc.fillCircle(cx, cy, 5, rgb(255, 210, 40)); for(int i = 0; i < 8; i++){ float a = i * 0.785f + now / 1500.0f; cc.drawLine(cx + 7 * cos(a), cy + 7 * sin(a), cx + 9 * cos(a), cy + 9 * sin(a), rgb(255, 210, 40)); } }
  else if(k == "moon"){ cc.fillCircle(cx, cy, 6, rgb(240, 240, 200)); cc.fillCircle(cx + 3, cy - 2, 5, COL_BG); }
  else if(k == "rain" || k == "storm"){
    cc.fillCircle(cx - 3, cy - 1, 4, rgb(170, 180, 200)); cc.fillCircle(cx + 3, cy - 2, 5, rgb(170, 180, 200));
    for(int i = 0; i < 3; i++) cc.drawFastVLine(cx - 4 + i * 4, cy + 4 + (int)((now / 120 + i) % 3), 3, rgb(80, 150, 255));
    if(k == "storm" && now % 1600 < 150) cc.fillTriangle(cx, cy + 2, cx + 4, cy + 2, cx - 1, cy + 10, rgb(255, 230, 60));
  }
  else if(k == "snow"){ for(int i = 0; i < 5; i++) cc.fillCircle(cx - 6 + i * 3, cy - 4 + (int)((now / 150 + i * 3) % 10), 1, TFT_WHITE); }
  else if(k == "cold"){ uint16_t c = rgb(120, 200, 255); cc.drawFastVLine(cx, cy - 7, 14, c); cc.drawFastHLine(cx - 7, cy, 14, c); cc.drawLine(cx - 5, cy - 5, cx + 5, cy + 5, c); cc.drawLine(cx - 5, cy + 5, cx + 5, cy - 5, c); }
  else if(k == "hot"){ cc.fillCircle(cx, cy, 6, rgb(255, 120, 30)); }
  else { cc.fillCircle(cx - 3, cy, 4, rgb(190, 195, 210)); cc.fillCircle(cx + 3, cy - 1, 5, rgb(190, 195, 210)); }   // nuvole/nebbia
}

// ---- barre e testi comuni ----
void compBar(int x, int y, int w, int h, int pct, uint16_t c){
  cc.fillRoundRect(x, y, w, h, h / 2, rgb(45, 48, 60));
  int f = constrain(pct, 0, 100) * w / 100;
  if(f > 0) cc.fillRoundRect(x, y, max(f, h), h, h / 2, c);
}
void compText(const char* s, int x, int y, uint16_t col, textdatum_t d = top_left, const lgfx::IFont* f = &fonts::Font0){
  cc.setTextFont(f); cc.setTextDatum(d); cc.setTextColor(col); cc.drawString(s, x, y);
}
String resetText(const String& r){ return r == "-" ? String("") : String("reset ") + r.substring(0, r.indexOf('_')) + " " + r.substring(r.indexOf('_') + 1); }

// Claude Code: al posto degli occhioni c'è la mascotte, il granchietto arancione del terminale
// (18 x 10 unità: corpo, braccini, quattro zampette, occhi). Pensa, ti cerca o ha finito.
const char* CLAWD_VERBS[] = {"Rimugino", "Elaboro", "Ragiono", "Macino", "Cucino", "Medito", "Combino"};
void compClawdBody(int x, int y, int u, const String& m, uint32_t now, float k){
  uint16_t c = rgb(217, 119, 87);
  int step = (now / 250) % 2;
  cc.fillRect(x + 3 * u, y, 12 * u, 8 * u, c);                     // corpo
  int left = m == "done" ? 2 * u : 0, right = left;
  if(m == "ask"){ left = step ? 2 * u : 0; right = 2 * u - left; }  // saluta con un braccino alla volta
  cc.fillRect(x + u, y + 4 * u - left, 2 * u, 2 * u, c);
  cc.fillRect(x + 15 * u, y + 4 * u - right, 2 * u, 2 * u, c);
  const int legs[4] = {4, 6, 11, 13};
  for(int i = 0; i < 4; i++){
    int h = m == "work" && i % 2 == step ? u : 2 * u;               // passetti
    cc.fillRect(x + legs[i] * u, y + 8 * u, u, h, c);
  }
  for(int col : {5, 12}){
    int ex = x + col * u, ey = y + 2 * u;
    if(k < 0.5f) cc.fillRect(ex, ey + u, u, 2, TFT_BLACK);              // battito di palpebre
    else if(m == "work") cc.fillRect(ex, ey + u, u, u, TFT_BLACK);      // occhi stretti, concentrato
    else if(m == "done"){                                               // occhi felici ^
      cc.fillTriangle(ex - u / 2, ey + 2 * u, ex + u / 2, ey, ex + u + u / 2, ey + 2 * u, TFT_BLACK);
      cc.fillTriangle(ex, ey + 2 * u, ex + u / 2, ey + u, ex + u, ey + 2 * u, c);
    } else cc.fillRect(ex, ey, u, 2 * u, TFT_BLACK);
  }
}
void compSpark(int cx, int cy, int r, uint16_t c, uint32_t now){   // la stellina che gira mentre pensa
  float a0 = now / 400.0f; int rr = r - (now / 120) % 3 * 2;
  for(int i = 0; i < 4; i++){
    float a = a0 + i * 3.14159f / 4;
    int dx = rr * cos(a), dy = rr * sin(a);
    cc.drawWideLine(cx - dx, cy - dy, cx + dx, cy + dy, 2, c);
  }
}
void compClawd(const String& m, uint32_t now, float k){
  const int u = 6, cx = FACE_W / 2;
  uint16_t orange = rgb(217, 119, 87);
  if(m == "work"){
    compClawdBody(cx - 9 * u - 30, 40 + (now / 250) % 2, u, m, now, k);
    compSpark(cx + 78, 62, 18, orange, now);
    String verb = String(CLAWD_VERBS[(now / 3000) % 7]) + String("...").substring(0, (now / 350) % 4);
    compText(verb.c_str(), cx - 30, 108, moodColor("work"), top_center, &fonts::Font2);
    if(now % 5000 < 1500) compSweat(cx - 30 + 9 * u + 10, 34 + (now % 5000) / 60);
  } else if(m == "ask"){
    compClawdBody(cx - 9 * u, 36 - ((now / 300) % 2 ? u : 0), u, m, now, k);
    if((now / 400) % 2) compText("?", cx + 9 * u + 14, 24, moodColor("ask"), top_center, &fonts::Font4);
    if((now / 400) % 2) compText("Ti cerco!", cx, 112, moodColor("ask"), top_center, &fonts::Font2);
  } else {
    compClawdBody(cx - 9 * u, 40 - abs((int)(10 * sin(now / 150.0))), u, m, now, k);
    for(int i = 0; i < 6; i++){                                     // scintille di festa
      if((now / 200 + i) % 3 == 0) continue;
      int sx = (i * 73 + 20) % 200 + 20, sy = 24 + (i * 37) % 70;
      if(abs(sx - cx) < 9 * u + 6) sx = sx < cx ? sx - 50 : sx + 50;
      compSpark(sx, sy, 6, hueColor(i * 60 + now / 10), now + i * 300);
    }
    compText("Fatto!", cx, 112, moodColor("done"), top_center, &fonts::Font2);
  }
}

// barra in alto: ora, titolo, batteria
void compTopBar(){
  cc.fillRect(0, 0, CW, CTOP, COL_BAR);
  compText(companionPcConnected() ? cs.clock.c_str() : clockStr().c_str(), 3, 3, TFT_WHITE);
  const char* title = compShowQr ? "INSTALLA L'APP" : compView == 1 ? "CONSUMI CLAUDE" : compView == 2 ? "PC" : "COMPANION";
  compText(title, CW / 2, 3, COL_ACCENT, top_center);
  int bx = CW - 24;
  cc.drawRoundRect(bx, 3, 18, 9, 2, TFT_WHITE); cc.fillRect(bx + 18, 5, 2, 5, TFT_WHITE);
  if(gLevel >= 0) cc.fillRect(bx + 2, 5, constrain(gLevel, 0, 100) * 14 / 100, 5, gLevel > 50 ? TFT_GREEN : (gLevel > 20 ? TFT_ORANGE : TFT_RED));
  if(gChg) compText("+", bx - 7, 3, TFT_YELLOW);
  compText(companionPcConnected() ? "PC" : "--", bx - 22, 3, companionPcConnected() ? rgb(80, 220, 110) : COL_MUTED);
}

// striscia in basso per musica/video: titolo che scorre e barra del tempo
void compMediaFooter(uint32_t now){
  if(cs.media < 0 || !cs.song.length()) return;
  uint16_t mc = cs.media ? hueColor(now / 12) : rgb(150, 200, 255);
  cc.fillRect(0, CH - 19, CW, 19, TFT_BLACK);
  cc.setClipRect(0, CH - 19, CW, 9);
  compText(cs.song.c_str(), CW - (int)((now / 30) % (cs.song.length() * 6 + CW)), CH - 19, mc);
  cc.clearClipRect();
  if(cs.mDur > 0){
    int cur = min(cs.mDur, cs.mPos + (int)((millis() - cs.mAt) / 1000));
    char a[8], b[8]; snprintf(a, 8, "%d:%02d", cur / 60, cur % 60); snprintf(b, 8, "%d:%02d", cs.mDur / 60, cs.mDur % 60);
    compText(a, 2, CH - 8, COL_MUTED); compText(b, CW - 2, CH - 8, COL_MUTED, top_right);
    compBar(34, CH - 7, CW - 68, 5, cur * 100 / cs.mDur, cs.media ? hueColor(now / 12 + 120) : rgb(150, 200, 255));
  }
}

// ---- feste: il PC manda "F <festa>", l'animazione si alterna con quella del meteo ----
bool compIsFeast(const String& k){
  return k == "halloween" || k == "natale" || k == "capodanno" || k == "befana" || k == "sanvalentino"
      || k == "carnevale" || k == "pasqua" || k == "italia" || k == "ferragosto";
}

// ---- viste di KEY2 ----
void compViewClaude(uint32_t now){
  cc.fillRect(0, CTOP, CW, CH - CTOP, COL_BG);
  const char* nm[] = {"5 ore", "Settimana"}; String v[] = {cs.u5, cs.u7}; String r[] = {cs.r5, cs.r7};
  for(int i = 0; i < 2; i++){
    int y = CTOP + 8 + i * 58, p = v[i] == "-" ? 0 : v[i].toInt();
    compText(nm[i], 8, y + 4, TFT_WHITE, top_left, &fonts::Font2);
    compText(v[i] == "-" ? "--" : (v[i] + "%").c_str(), CW - 8, y, levelColor(p), top_right, &fonts::Font4);
    compBar(8, y + 28, CW - 16, 10, p, levelColor(p));
    compText(resetText(r[i]).c_str(), 8, y + 42, COL_MUTED);
  }
}
void compViewClock(uint32_t now){
  cc.fillRect(0, CTOP, CW, CH - CTOP, COL_BG);
  String c = companionPcConnected() ? cs.clock : clockStr();
  if((now / 500) % 2) c.replace(":", " ");
  compText(c.c_str(), CW / 2, CTOP + 6, TFT_WHITE, top_center, &fonts::Font7);
  const char* nm[] = {"CPU", "RAM", "GPU"}; int v[] = {cs.cpu, cs.ram, cs.gpu};
  for(int i = 0; i < 3; i++){
    int x = 6 + i * 78, y = CTOP + 66;
    compText(nm[i], x, y, COL_MUTED, top_left, &fonts::Font2);
    char t[8]; if(v[i] < 0) strcpy(t, "--"); else snprintf(t, 8, "%d%%", v[i]);
    compText(t, x + 70, y, TFT_WHITE, top_right, &fonts::Font2);
    compBar(x, y + 20, 70, 8, max(0, v[i]), levelColor(max(0, v[i])));
  }
  int y = CTOP + 102;
  if(cs.wx.length()){ compWxIcon(cs.wx, 16, y + 8, now); char t[8]; snprintf(t, 8, "%dC", cs.wxTemp); compText(t, 30, y + 1, TFT_WHITE, top_left, &fonts::Font2); }
  if(cs.gtemp >= 0){ char t[16]; snprintf(t, 16, "GPU %dC", cs.gtemp); compText(t, CW - 6, y + 1, cs.gtemp >= 80 ? rgb(255, 70, 60) : COL_MUTED, top_right, &fonts::Font2); }
}

// ---- schermata: nessun dato dal PC (o QR richiesto) -> installa l'app ----
void compInstall(uint32_t now){
  cc.fillRect(0, CTOP, CW, CH - CTOP, COL_BG);
  cc.fillRoundRect(4, CTOP + 3, 114, 114, 6, TFT_WHITE);
  cc.qrcode(COMP_REPO_URL, 9, CTOP + 8, 104, 4);
  int x = 122;
  compText("Desktop Companion", x, CTOP + 3, COL_ACCENT, top_left, &fonts::Font2);
  bool pc = companionPcConnected();
  if(pc){
    compText("App gia' installata:", x, CTOP + 24, TFT_WHITE);
    compText("KEY2 breve = indietro", x, CTOP + 36, TFT_WHITE);
    compText("KEY2 lungo = aggiorna", x, CTOP + 46, TFT_WHITE);
  } else if(compKeyboardReady()){
    compText("Bluetooth collegato!", x, CTOP + 24, rgb(80, 220, 110));
    compText("Tieni premuto KEY2:", x, CTOP + 36, TFT_WHITE);
    compText("apro sul PC la pagina", x, CTOP + 46, TFT_WHITE);
    compText("con le istruzioni", x, CTOP + 56, TFT_WHITE);
  } else {
    compText("Associa 'Nesso N1'", x, CTOP + 24, TFT_WHITE);
    compText("nel Bluetooth di", x, CTOP + 34, TFT_WHITE);
    compText("Windows, poi tieni", x, CTOP + 44, TFT_WHITE);
    compText("premuto KEY2.", x, CTOP + 54, TFT_WHITE);
    compText("Oppure inquadra il QR", x, CTOP + 68, COL_MUTED);
  }
  compText("github.com/AndreaDp271", x, CTOP + 84, COL_MUTED);
  compText("/Companion-Robot", x, CTOP + 94, COL_MUTED);
  String dots = String("in attesa del PC") + String("...").substring(0, (now / 400) % 4);
  compText(pc ? "PC collegato" : dots.c_str(), x, CTOP + 108, pc ? rgb(80, 220, 110) : rgb(255, 150, 40));
}

// scrittura del comando in corso / appena finita
void compTypingScreen(uint32_t now){
  cc.fillRect(0, CTOP, CW, CH - CTOP, COL_BG);
  bool done = compTyping < 0;
  scnBegin(); scnFace(done ? "happy" : "work", now / 1000.0f, 0, -28, 0.55f); scnEnd();
  compText(done ? "Pagina aperta sul PC!" : "Apro la pagina GitHub...", CW / 2, 88, TFT_WHITE, top_center, &fonts::Font2);
  if(!done) compBar(20, 110, CW - 40, 8, compTyping, moodColor("work"));
  else compText("segui le istruzioni nel browser", CW / 2, 110, COL_MUTED, top_center);
}

// ---- pagina ----
void renderCompanion(){
  if(!ccReady){ cc.createSprite(CW, CH); ccReady = true; }
  uint32_t now = millis();
  if(compView && (int32_t)(now - compViewUntil) > 0) compView = 0;
  cc.fillSprite(TFT_BLACK);
  compTopBar();
  bool justInstalled = compInstallAt && now - compInstallAt < 15000 && !companionPcConnected();
  if(compTyping >= 0 || justInstalled){ compTypingScreen(now); cc.pushSprite(0, 0); return; }
  if(!companionPcConnected() || compShowQr){ compInstall(now); cc.pushSprite(0, 0); return; }
  if(compView == 1){ compViewClaude(now); cc.pushSprite(0, 0); return; }
  if(compView == 2){ compViewClock(now); cc.pushSprite(0, 0); return; }

  String m = compMood();
  float k = compBlink(now);   // battito di palpebre della mascotte di Claude
  const int cx = FACE_W / 2;

  // meteo: ogni tanto un'animazione, quando il companion non sta facendo altro di importante
  bool speaking = cs.msgText.length() && (int32_t)(cs.msgUntil - now) > 0;
  bool wxEligible = m == "normal" || m == "happy" || m == "stress" || m == "code" || m == "bored" || m == "sleepy";
  if(wxa.kind.length() && (int32_t)(now - wxa.until) > 0) wxa.kind = "";
  if(!wxa.kind.length() && wxEligible && !speaking && (cs.wx.length() || compFest.length())){
    if(!wxa.next) wxa.next = now + 15000;                         // la prima poco dopo l'avvio
    else if((int32_t)(now - wxa.next) > 0){
      static bool festTurn = false; festTurn = !festTurn;         // nei giorni di festa si alterna con il meteo
      wxa.kind = compFest.length() && (festTurn || !cs.wx.length()) ? compFest : cs.wx;
      wxa.until = now + WX_ANIM_MS; wxa.forced = false;
      wxa.next = now + random(240000, 420000);                    // poi ogni 4-7 minuti
    }
  }
  bool wxOn = wxa.kind.length() && !speaking && m != "ask" && (wxa.forced || wxEligible);

  const float t = now / 1000.0f;
  if(compUpdAt && now - compUpdAt < 10000 && !speaking){
    // aggiornamento chiesto al PC, in attesa della risposta
    scnBegin(); scnFace("surprised", t, 0, -20, 0.6f); scnEnd();
    compText("Chiedo al PC", cx, 92, TFT_WHITE, top_center, &fonts::Font2);
    compText(("di aggiornare" + String("...").substring(0, (now / 300) % 4)).c_str(), cx, 110, TFT_WHITE, top_center, &fonts::Font2);
  } else if(!(speaking && m != "ask") && (m == "work" || m == "ask" || m == "done")){   // Claude Code: la mascotte
    compClawd(m, now, k);
  } else {
    scnBegin();
    if(speaking) scnSpeech(cs.msgEmo, cs.msgText, t);
    else if(wxOn) compIsFeast(wxa.kind) ? scnFeast(wxa.kind, t) : scnWeather(wxa.kind, t);
    else if(m == "music") scnMusic(t);
    else if(m == "video") scnVideo(t);
    else if(m == "code") scnCode(t, cs.media == 1);
    else if(m == "game") scnGame(cs.detail, t);
    else if(m == "osm") scnOsm(cs.detail == "edit", t);
    else scnMood(m.c_str(), t);
    scnEnd();
  }
  compMediaFooter(now);
  compTopBar();   // per ultima: gli archetti delle cuffie non la coprono
  cc.pushSprite(0, 0);
}
