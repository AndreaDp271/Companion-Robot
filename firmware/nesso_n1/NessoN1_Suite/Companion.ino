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
    case 'H': if(n == 1){ cs.media = p[0] == "1" ? 1 : (p[0] == "0" ? 0 : -1); if(cs.media < 0){ cs.song = ""; cs.mDur = 0; } } break;
    case 'B': if(n == 2){ cs.mPos = p[0].toInt(); cs.mDur = p[1].toInt(); cs.mAt = millis(); } break;
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
      if(compLine == "#shot") compScreenshot(); else compParse(compLine);
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

// ---- occhi ----
void drawHeart(int cx, int cy, int s, uint16_t c){
  int r = s / 4;
  cc.fillCircle(cx - r, cy - r / 2, r, c);
  cc.fillCircle(cx + r, cy - r / 2, r, c);
  cc.fillTriangle(cx - 2 * r, cy - r / 3, cx + 2 * r, cy - r / 3, cx, cy + 2 * r, c);
}
// forma base colorata + palpebre/effetti disegnati col colore di sfondo
void compEye(int cx, int cy, int w, int h, const String& m, int side, uint16_t col){
  const uint16_t bg = TFT_BLACK;
  if(m == "love" && h > 10){ drawHeart(cx, cy, w, col); return; }
  if((m == "surprised" || m == "ask") && h > 10){
    cc.fillEllipse(cx, cy, w / 2, h / 2, col);
    cc.fillCircle(cx, cy, w / 5, bg);
    cc.fillCircle(cx - w / 12, cy - w / 12, max(2, w / 14), TFT_WHITE);
    return;
  }
  int x = cx - w / 2, y = cy - h / 2;
  cc.fillRoundRect(x, y, w, h, min(12, min(w, h) / 2), col);
  if(h <= 10) return;
  if(m == "happy" || m == "done" || m == "excited"){            // occhio ad arco ^
    cc.fillEllipse(cx, y + h + 6, w / 2 + 6, h / 2 + 2, bg);
  } else if(m == "stress" || m == "game" || m == "work"){      // sopracciglia aggrottate
    if(m == "work") cc.fillRect(x, y, w, h * 3 / 10, bg);
    if(side < 0) cc.fillTriangle(x, y - 1, x + w, y - 1, x + w, y + h / 2, bg);
    else         cc.fillTriangle(x, y - 1, x + w, y - 1, x, y + h / 2, bg);
  } else if(m == "sad" || m == "sleepy"){                       // palpebra che scende verso l'esterno
    if(side < 0) cc.fillTriangle(x, y - 1, x + w, y - 1, x, y + h * 6 / 10, bg);
    else         cc.fillTriangle(x, y - 1, x + w, y - 1, x + w, y + h * 6 / 10, bg);
    if(m == "sleepy") cc.fillRect(x, y, w, h * 35 / 100, bg);
  } else if(m == "bored"){
    cc.fillRect(x, y, w, h * 55 / 100, bg);
  } else {                                                       // riflesso: occhi più vivi
    cc.fillCircle(x + w / 4, y + h / 5 + 2, max(2, w / 9), rgb(230, 250, 255));
  }
}
void compEyes(int cx1, int cx2, int cy, int w, int h, const String& m, uint16_t col){
  compEye(cx1, cy, w, h, m, -1, col);
  compEye(cx2, cy, w, h, m, 1, col);
}

// ---- accessori delle scene (area occhi: x 0..150, y 14..135) ----
void compThought(uint32_t now){   // Claude pensa: nuvoletta con i puntini
  uint16_t c = rgb(235, 235, 245);
  const int X = 90;   // in alto a destra
  cc.fillCircle(X + 118, 54, 3, c); cc.fillCircle(X + 125, 44, 4, c);
  cc.fillCircle(X + 122, 29, 10, c); cc.fillCircle(X + 134, 25, 10, c); cc.fillCircle(X + 141, 33, 7, c); cc.fillCircle(X + 130, 36, 9, c);
  for(int i = 0; i < (int)((now / 350) % 4); i++) cc.fillCircle(X + 123 + i * 6, 30, 2, rgb(180, 110, 255));
}
void compSweat(int x, int y){
  uint16_t c = rgb(120, 200, 255);
  cc.fillCircle(x, y, 3, c); cc.fillTriangle(x - 3, y - 1, x + 3, y - 1, x, y - 8, c);
}
void compHeadphones(int cx, int cy, int half, uint16_t cup){
  cc.fillArc(cx, cy, half, half - 4, 180, 360, rgb(90, 90, 100));   // archetto
  cc.fillRoundRect(cx - half - 6, cy - 12, 12, 26, 5, cup);
  cc.fillRoundRect(cx + half - 6, cy - 12, 12, 26, 5, cup);
  cc.fillRect(cx - half - 1, cy - 8, 2, 18, rgb(40, 40, 50));
  cc.fillRect(cx + half - 1, cy - 8, 2, 18, rgb(40, 40, 50));
}
void compNote(int x, int y, uint16_t c){
  cc.fillCircle(x, y, 3, c); cc.fillRect(x + 2, y - 10, 2, 10, c); cc.fillTriangle(x + 3, y - 10, x + 8, y - 7, x + 3, y - 5, c);
}
void compLaptop(uint32_t now){   // VS Code: portatile con codice colorato
  const int X = XO;
  cc.fillRoundRect(X + 20, 64, 111, 58, 3, rgb(60, 60, 70));
  cc.fillRect(X + 23, 67, 105, 52, rgb(30, 30, 40));
  const uint16_t cols[] = { rgb(200, 120, 255), rgb(120, 200, 255), rgb(240, 200, 90), rgb(130, 220, 130), rgb(230, 110, 110) };
  int scroll = (now / 600) % 7;
  for(int i = 0; i < 7; i++){
    int seed = (i + scroll) * 37;
    int ind = (seed % 3) * 6, len1 = 10 + seed % 22, len2 = 8 + (seed / 3) % 28;
    cc.fillRect(X + 28 + ind, 71 + i * 7, len1, 3, cols[seed % 5]);
    if(28 + ind + len1 + 4 + len2 < 125) cc.fillRect(X + 28 + ind + len1 + 4, 71 + i * 7, len2, 3, cols[(seed / 5) % 5]);
  }
  if((now / 300) % 2) cc.fillRect(X + 28 + ((now / 600) % 6) * 14, 113, 6, 3, TFT_WHITE);   // cursore
  cc.fillRoundRect(X + 10, 122, 131, 6, 2, rgb(150, 150, 160));
}
void compPopcorn(int x, int y, uint32_t now){
  cc.fillTriangle(x, y, x + 26, y, x + 22, y + 30, rgb(230, 50, 50));
  cc.fillTriangle(x, y, x + 22, y + 30, x + 4, y + 30, rgb(230, 50, 50));
  for(int i = 0; i < 3; i++) cc.fillRect(x + 4 + i * 8, y + 2, 3, 27, TFT_WHITE);
  const int8_t k[][2] = {{3, -2}, {10, -5}, {17, -3}, {23, -1}, {13, 0}};
  for(auto& p : k) cc.fillCircle(x + p[0], y + p[1], 4, rgb(255, 240, 190));
  float t = (now % 1400) / 1400.0f;   // chicco che salta
  cc.fillCircle(x + 14 - (int)(18 * t), y - 6 - (int)(34 * t - 36 * t * t), 3, rgb(255, 240, 190));
}
void compGameIcon(const String& g, uint32_t now){   // icona del gioco sotto gli occhi
  int cx = FACE_W / 2, cy = 98;
  if(g == "minecraft"){
    int s = 32, x = cx - s / 2, y = cy - s / 2 + (int)(2 * sin(now / 300.0));
    cc.fillRect(x, y, s, s, rgb(134, 96, 67)); cc.fillRect(x, y, s, 8, rgb(95, 180, 60));
    for(int i = 0; i < 12; i++) cc.fillRect(x + (i * 7) % (s - 3), y + 10 + (i * 11) % (s - 13), 3, 3, rgb(100, 70, 50));
    for(int i = 0; i < 6; i++) cc.fillRect(x + i * 5, y + 7, 3, 3 + (i % 2) * 2, rgb(95, 180, 60));
  } else if(g == "sot"){
    int b = (int)(3 * sin(now / 380.0));
    cc.fillRect(cx + 2, cy - 24 + b, 2, 24, rgb(120, 80, 40));
    cc.fillRect(cx - 12, cy - 22 + b, 28, 14, rgb(240, 240, 230));
    cc.fillCircle(cx + 2, cy - 15 + b, 3, TFT_BLACK);
    cc.fillTriangle(cx - 30, cy + b, cx + 30, cy + b, cx + 21, cy + 10 + b, rgb(140, 90, 45));
    cc.fillTriangle(cx - 30, cy + b, cx + 21, cy + 10 + b, cx - 21, cy + 10 + b, rgb(140, 90, 45));
    for(int x = 0; x < FACE_W; x += 3) cc.fillRect(x, cy + 13 + (int)(2 * sin(x / 7.0 + now / 220.0)), 3, 3, rgb(40, 140, 220));
  } else if(g == "cities"){
    const int8_t hs[] = {20, 32, 24, 38, 28, 18, 26};
    for(int i = 0; i < 7; i++){
      int x = XO + 8 + i * 20, h = hs[i], y = cy + 26 - h;
      cc.fillRect(x, y, 16, h, rgb(110, 115, 130));
      for(int wy = y + 3; wy < cy + 23; wy += 5) for(int wx = x + 2; wx < x + 14; wx += 4)
        cc.fillRect(wx, wy, 2, 2, ((wx * 7 + wy * 13 + now / 900) % 3) ? rgb(250, 210, 60) : rgb(50, 50, 60));
    }
  } else if(g == "ets2"){
    for(int x = -((int)(now / 20) % 16); x < FACE_W; x += 16) cc.fillRect(x, cy + 24, 8, 2, TFT_WHITE);
    cc.fillRect(XO + 22, cy - 14, 64, 28, rgb(230, 230, 235));
    cc.fillRoundRect(XO + 88, cy - 8, 34, 22, 4, rgb(255, 140, 30));
    cc.fillRect(XO + 97, cy - 5, 16, 8, rgb(120, 200, 255));
    int sp = (now / 60) % 4;
    for(int wx : {32, 50, 74, 98, 114}){ wx += XO; cc.fillCircle(wx, cy + 17, 5, rgb(40, 40, 40)); cc.fillCircle(wx + (sp == 1 ? 2 : sp == 3 ? -2 : 0), cy + 17 + (sp == 0 ? -2 : sp == 2 ? 2 : 0), 1, rgb(200, 200, 200)); }
  } else if(g == "isonzo"){
    uint32_t t = now % 3000;
    if(t < 300) cc.fillCircle(XO + 24, cy - 10, 10 + t / 20, rgb(255, 150, 40));
    cc.fillArc(cx, cy + 12, 28, 0, 180, 360, rgb(110, 110, 70));
    cc.fillRect(cx - 36, cy + 11, 72, 4, rgb(90, 90, 55));
    cc.fillRect(cx - 1, cy - 20, 2, 6, rgb(90, 90, 55));
  } else if(g == "tlou"){
    float a = -0.25f + 0.3f * sin(now / 1400.0);
    int ox = XO + 14, oy = cy + 8;
    for(int r = 0; r < 130; r += 2){
      int y1 = oy + (int)(r * tan(a - 0.3f)), y2 = oy + (int)(r * tan(a + 0.3f));
      int top = max(y1, 66), bot = min(y2, CH);
      if(bot > top) cc.drawFastVLine(ox + r, top, bot - top, rgb(max(10, 80 - r / 2), max(10, 75 - r / 2), 20));
    }
    int zx = XO + 130 - (int)((now / 40) % 90);   // clicker che avanza
    uint16_t z = rgb(150, 170, 110);
    cc.fillCircle(zx, cy - 8, 5, z); cc.fillRect(zx - 3, cy - 3, 7, 16, z);
    for(int d = -6; d <= 6; d += 4) cc.drawLine(zx, cy - 8, zx + d, cy - 15, z);
    cc.fillRect(ox - 8, oy - 3, 10, 6, rgb(200, 200, 210));
  } else {   // gioco sconosciuto: gamepad
    cc.fillRoundRect(cx - 34, cy - 13, 68, 28, 12, rgb(70, 70, 85));
    cc.fillRect(cx - 24, cy - 2, 14, 4, TFT_WHITE); cc.fillRect(cx - 19, cy - 7, 4, 14, TFT_WHITE);
    const uint16_t bc[] = {rgb(80, 220, 110), rgb(255, 70, 60), rgb(80, 150, 255), rgb(250, 210, 60)};
    const int8_t bp[][2] = {{0, -6}, {6, 0}, {0, 6}, {-6, 0}};
    int pressed = (now / 200) % 4;
    for(int i = 0; i < 4; i++) cc.fillCircle(cx + 19 + bp[i][0], cy + bp[i][1], i == pressed ? 2 : 3, bc[i]);
  }
}
void compOsm(bool edit, uint32_t now){   // OpenStreetMap: mappetta colorata
  int x0 = 4, y0 = 64, w = FACE_W - 8, h = 66;
  cc.fillRect(x0, y0, w, h, rgb(242, 239, 233));                    // sfondo mappa OSM
  cc.fillRoundRect(10, y0 + 6, 34, 22, 4, rgb(173, 209, 158));       // parco
  for(int x = x0; x < x0 + w; x++) cc.fillRect(x, y0 + 42 + (int)(5 * sin(x / 12.0)), 1, 6, rgb(170, 211, 223));   // fiume
  cc.fillRect(x0, y0 + 30, w, 5, rgb(247, 250, 191)); cc.drawRect(x0, y0 + 30, w, 5, rgb(200, 180, 120));   // strada
  cc.fillRect(XO + 58, y0, 4, h, TFT_WHITE); cc.drawRect(XO + 58, y0, 4, h, rgb(190, 190, 190));
  cc.fillRoundRect(XO + 150, y0 + 40, 30, 16, 4, rgb(173, 209, 158));   // secondo parco
  if(!edit){
    int py = y0 + 12 - abs((int)(4 * sin(now / 200.0)));   // segnaposto che rimbalza
    cc.fillTriangle(XO + 100, py + 4, XO + 110, py + 4, XO + 105, py + 16, rgb(230, 60, 60));
    cc.fillCircle(XO + 105, py + 3, 6, rgb(230, 60, 60)); cc.fillCircle(XO + 105, py + 3, 2, TFT_WHITE);
  } else {   // modifica: edificio disegnato nodo per nodo
    const uint8_t pts[][2] = {{78, 4}, {130, 4}, {130, 26}, {104, 26}, {104, 40}, {78, 40}};
    int n = (now / 450) % 10;
    uint16_t o = rgb(255, 140, 30);
    if(n > 6){ cc.fillRect(XO + 79, y0 + 5, 51, 21, rgb(255, 200, 150)); cc.fillRect(XO + 79, y0 + 26, 25, 14, rgb(255, 200, 150)); }
    for(int i = 1; i < min(n + 1, 7); i++){
      int a = i - 1, b = i % 6;
      cc.drawLine(XO + pts[a][0], y0 + pts[a][1], XO + pts[b][0], y0 + pts[b][1], o);
      cc.drawLine(XO + pts[a][0], y0 + pts[a][1] + 1, XO + pts[b][0], y0 + pts[b][1] + 1, o);
    }
    for(int i = 0; i < min(n + 1, 6); i++){ cc.fillRect(XO + pts[i][0] - 2, y0 + pts[i][1] - 2, 5, 5, TFT_WHITE); cc.drawRect(XO + pts[i][0] - 2, y0 + pts[i][1] - 2, 5, 5, o); }
  }
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

// ---- nuvoletta con la frase ----
void compSpeech(const String& emo, const String& text, float k, uint32_t now){
  cc.fillRoundRect(3, CTOP + 3, FACE_W - 6, 40, 8, TFT_WHITE);
  cc.fillTriangle(FACE_W / 2 - 12, CTOP + 41, FACE_W / 2 + 4, CTOP + 41, FACE_W / 2 - 10, CTOP + 51, TFT_WHITE);
  cc.setTextColor(TFT_BLACK, TFT_WHITE); cc.setTextDatum(middle_center); cc.setTextFont(&fonts::Font2);
  String l1 = text, l2 = "";   // a capo sulla parola, max 2 righe da ~26 caratteri
  if(text.length() > 26){
    int cut = text.lastIndexOf(' ', 26); if(cut < 0) cut = 26;
    l1 = text.substring(0, cut); l2 = text.substring(cut + (text[cut] == ' ' ? 1 : 0));
    if(l2.length() > 27) l2 = l2.substring(0, 27);
  }
  if(l2.length()){ cc.drawString(l1.c_str(), FACE_W / 2, CTOP + 14); cc.drawString(l2.c_str(), FACE_W / 2, CTOP + 32); }
  else cc.drawString(l1.c_str(), FACE_W / 2, CTOP + 23);
  int bounce = (emo == "excited" || emo == "love" || emo == "happy") ? -abs((int)(3 * sin(now / 140.0))) : 0;
  compEyes(FACE_W / 2 - 32, FACE_W / 2 + 32, 98 + bounce, 40, max(2, (int)(36 * k)), emo, moodColor(emo));
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
  compEyes(80, 160, 56, 40, 46, done ? "happy" : "work", done ? moodColor("done") : moodColor("work"));
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
  compLook(now, m);
  float k = compBlink(now);
  int ox = (int)fx.lx, oy = (int)fx.ly;
  const int cx = FACE_W / 2;

  if(compUpdAt && now - compUpdAt < 10000 && !(cs.msgText.length() && (int32_t)(cs.msgUntil - now) > 0)){
    // aggiornamento chiesto al PC, in attesa della risposta
    compEyes(cx - 34, cx + 34, 58, 40, 46, "surprised", moodColor("excited"));
    compText("Chiedo al PC", cx, 92, TFT_WHITE, top_center, &fonts::Font2);
    compText(("di aggiornare" + String("...").substring(0, (now / 300) % 4)).c_str(), cx, 110, TFT_WHITE, top_center, &fonts::Font2);
  } else if(cs.msgText.length() && (int32_t)(cs.msgUntil - now) > 0 && m != "ask"){
    compSpeech(cs.msgEmo, cs.msgText, k, now);
  } else if(m == "music"){   // cuffie e occhi arcobaleno che ballano a tempo
    float beat = (now % 500) / 500.0f; int n = now / 500;
    int bounce = -(int)(6 * sin(3.14159f * beat)), sway = (int)(14 * sin(now * 3.14159f / 1000.0f));
    const char* dance[] = {"normal", "happy", "normal", "love"};
    int h = beat < 0.12f ? 44 : 52;
    compEyes(cx - 28 + sway, cx + 28 + sway, 74 + bounce, 42, max(2, (int)(h * k)), dance[(n / 4) % 4], hueColor(now / 8));
    compHeadphones(cx + sway, 74 + bounce, 58, hueColor(now / 8 + 180));
    for(int i = 0; i < 6; i++){   // note colorate che salgono
      int t = (now / 20 + i * 27) % 160, side = i % 2 ? 1 : -1;
      compNote(cx + side * (72 + t / 5 + (i / 2) * 6), 108 - t * 2 / 3, hueColor(i * 60 + now / 10));
    }
  } else if(m == "video"){
    int fl = (now / 180) % 5 == 0 ? 3 : 0;
    compEyes(cx - 44 + ox / 3, cx + 12 + ox / 3, 62, 44, max(2, (int)((52 - fl) * k)), "normal", fl ? rgb(200, 230, 255) : moodColor("video"));
    compPopcorn(cx + 56, 76, now);
  } else if(m == "code"){
    compEyes(cx - 30 + ox / 2, cx + 30 + ox / 2, 38 + oy / 3, 38, max(2, (int)(30 * k)), "normal", moodColor("code"));
    if(cs.media == 1) compHeadphones(cx + ox / 2, 40, 58, rgb(255, 90, 170));
    compLaptop(now);
  } else if(m == "game"){
    compEyes(cx - 30 + ox / 2, cx + 30 + ox / 2, 38 + oy / 3, 38, max(2, (int)(32 * k)), "game", gameColor(cs.detail));
    compGameIcon(cs.detail, now);
  } else if(m == "osm"){
    compEyes(cx - 30 + ox / 2, cx + 30 + ox / 2, 38 + oy / 3, 38, max(2, (int)(32 * k)), "normal", moodColor("osm"));
    compOsm(cs.detail == "edit", now);
  } else if(m == "work"){   // super concentrato
    compEyes(cx - 52 + ox, cx + 8 + ox, 80 + oy, 46, max(2, (int)(50 * k)), "work", moodColor("work"));
    compThought(now);
    if(now % 5000 < 1500) compSweat(cx - 88, 56 + (now % 5000) / 50);
  } else if(m == "ask"){
    int b = (int)(3 * sin(now / 120.0));
    compEyes(cx - 42, cx + 42, 64 + b, 62, 74, "ask", (now / 300) % 2 ? moodColor("ask") : rgb(255, 200, 120));
    if((now / 400) % 2) compText("Ti cerco!", cx, 114, moodColor("ask"), top_center, &fonts::Font2);
  } else {
    int b = m == "done" ? -abs((int)(5 * sin(now / 150.0))) : 0;
    int h = m == "sleepy" || m == "bored" ? 60 : 76;
    compEyes(cx - 46 + ox, cx + 46 + ox, 72 + oy + b, 62, max(2, (int)(h * k)), m, moodColor(m));
    if(m == "done") compText("Fatto!", cx, 116, moodColor("done"), top_center, &fonts::Font2);
  }
  compMediaFooter(now);
  compTopBar();   // per ultima: gli archetti delle cuffie non la coprono
  cc.pushSprite(0, 0);
}
