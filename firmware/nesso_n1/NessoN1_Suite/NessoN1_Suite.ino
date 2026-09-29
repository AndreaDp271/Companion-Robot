/*
 * ================================================================
 *  Arduino Nesso N1 - SUITE (verticale, KEY1 scorre le pagine)
 * ================================================================
 *  Schermo ST7789 1.14" 135x240 -> orientamento VERTICALE.
 *  Niente menu: KEY1 (BtnA) passa alla pagina successiva.
 *  KEY2 (BtnB) = azione della pagina corrente.
 *  Barra in alto su ogni pagina: ORA (sx) + BATTERIA (dx).
 *
 *  Pagine (in ordine, scorri con KEY1):
 *    0) COMPANION: occhioni che reagiscono al PC (app Desktop Companion via USB), in ORIZZONTALE.
 *                 KEY2 breve = vista (occhi/consumi/orologio), lungo = aggiorna o installa l'app
 *                 github.com/AndreaDp271/Companion-Robot  (vedi Companion.ino)
 *    1) DISEGNO : lavagna a dito, tavolozza colori. KEY2 = pulisci
 *    2) BOLLA   : livella a bolla / inclinometro. KEY2 = cambia modo
 *    3) WIFI    : scansione reti con barre + RSSI. KEY2 = scan
 *    4) LORA    : sniffer Meshtastic, legge i messaggi (chiave default). KEY2 = preset
 *    5) IR      : spegni/accendi TV multi-marca. KEY2 = commuta, tocco = invia
 *    6) TORCIA  : schermo chiaro. KEY2 = intensita' (tonalita', il backlight non si dima)
 *    7) CRONO   : cronometro. KEY2 = avvia/pausa, tocco = azzera
 *    8) GIOCO   : biglia controllata con l'inclinazione. KEY2 = reset
 *    9) MOUSE   : air-mouse BLE HID (si accoppia a Windows come mouse). KEY2 = click, tocco = blocca/sblocca puntatore
 *   10) GPS     : Quectel L80-M39 su UART (header 8 pin in alto). KEY2 = cambia vista (stato/posizione), tocco = sincronizza ora
 *   11) METEO   : OpenWeatherMap via WIFI + posizione GPS. KEY2 = aggiorna
 *   12) SETUP   : KEY2 cicla ORA / BATTERIA / RISPARMIO / WIFI (config via BLE)
 *
 *  NB: KEY1/KEY2 sono su IO expander I2C -> presenti con debounce anti-glitch.
 *
 * ----------------------------------------------------------------
 *  LIBRERIE (Arduino IDE 2.0)
 *    Board: ArduinoNessoN1 (package M5Stack >= 3.2.5)
 *    M5Unified>=0.2.11, M5GFX>=0.2.17, RadioLib>=7.3.0,
 *    IRremoteESP8266>=2.8.6 (per ESP32-C6 usa una versione recente)
 *    NimBLE-Arduino (h2zero) -> per la pagina MOUSE (air-mouse BLE HID).
 *    L'ESP32-C6 supporta solo lo stack NimBLE (niente Bluedroid classico),
 *    quindi il mouse HID e' implementato qui a mano su NimBLEHIDDevice,
 *    senza dipendere dalla libreria "ESP32 BLE Mouse" di T-vK (incompatibile).
 *    TinyGPSPlus (Mikal Hart) -> per la pagina GPS (parsing NMEA).
 *    ArduinoJson (Benoit Blanchon) -> per la pagina METEO (parsing risposta OpenWeatherMap).
 *    Preferences, HTTPClient, WiFiClientSecure -> incluse nel core ESP32, nessuna installazione extra.
 *
 *  >>> Per NTP/METEO: metti qui SSID e password della rete di default <<<
 *      (possono anche essere sovrascritte via BLE dalla pagina SETUP > WIFI,
 *      senza dover ricompilare: vedi la sezione "CREDENZIALI WIFI" piu' sotto)
 *
 *  >>> GPS (Quectel L80-M39) sull'header di espansione a 8 pin in alto:
 *      GND (pin1) -> GND modulo | +3V3 OUT (pin7) -> VCC modulo
 *      GPIO07/D1  (pin3) -> RX ESP32  <- TX modulo
 *      GPIO02/D2  (pin4) -> TX ESP32  -> RX modulo
 *      Default modulo: 9600 baud, NMEA, 1 Hz.
 */
// Credenziali in secrets.h (escluso da Git: copia secrets.example.h in secrets.h e compila).
// Senza secrets.h lo sketch compila lo stesso: WiFi/meteo restano da configurare (via BLE).
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID  ""
#define WIFI_PASS  ""
#define OWM_API_KEY ""
#endif
#define TZ_INFO    "CET-1CEST,M3.5.0,M10.5.0/3"   // ora legale Italia

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <time.h>
#include <sys/time.h>
#include "mbedtls/aes.h"   // decifra i pacchetti Meshtastic (AES-CTR)
// air-mouse: mouse BLE HID scritto direttamente su NimBLE-Arduino. La libreria
// "ESP32 BLE Mouse" di T-vK usa lo stack Bluedroid classico (BLEHIDDevice.h di
// esp32-BLE-Arduino) che NON esiste sull'ESP32-C6 (solo NimBLE) -> non compila.
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include <TinyGPSPlus.h>   // parsing NMEA per la pagina GPS (Quectel L80-M39)

// ---- dimensioni schermo (verticale) ----
const int W = 135, H = 240, BAR_H = 22, PAL_H = 24;
#define COL_BG   0x10A2   // grigio bluastro scuro
#define COL_BAR  0x18C3
#define COL_PANEL  0x18E3   // pannelli / righe di lista
#define COL_ACCENT TFT_CYAN // accento: titolo, selezione, pagina attiva
#define COL_MUTED  0x6B4D   // testo secondario
#define COL_LINE   0x3186   // linee, griglie, indicatori spenti

M5Canvas canvas(&M5.Display);

// Struct in alto: l'IDE Arduino genera i prototipi in cima al file, quindi i tipi
// usati come parametri (TBtn, Btn) devono essere dichiarati prima di ogni funzione.
struct TBtn { int x, y, w, h; const char* lbl; uint16_t col; };
struct Btn  { bool raw=false, state=false; uint32_t t=0; };   // debounce pulsanti
bool hit(const TBtn& b, int px, int py) { return px>=b.x && px<b.x+b.w && py>=b.y && py<b.y+b.h; }

// ---- pagine ----
// PG_COMP = Desktop Companion (occhioni che reagiscono al PC), vedi Companion.ino
enum { PG_COMP, PG_DRAW, PG_LEVEL, PG_WIFI, PG_LORA, PG_IR, PG_TORCH, PG_CRONO, PG_GAME, PG_MOUSE, PG_GPS, PG_WEATHER, PG_SETUP, PG_COUNT };
int page = PG_COMP;
const char* TITLE[] = { "COMPANION", "DISEGNO", "BOLLA", "WIFI", "LORA", "IR", "TORCIA", "CRONO", "GIOCO", "MOUSE", "GPS", "METEO", "SETUP" };

// ---- stato globali ----
bool touchPrev = false;
int  gLevel = -1; bool gChg = false;
uint32_t lastBlink = 0; bool ledOn = false;
void setLed(bool on){ M5.getIOExpander(1).digitalWrite(7, on ? false : true); }

// --- debounce robusto per KEY1/KEY2 (struct Btn dichiarata in cima) ---
// I pulsanti sono su un IO expander letto via I2C: i picchi di corrente RF di
// WiFi/LoRa possono far tornare una lettura sporca (spesso 0x00 = tutti premuti),
// generando un falso "premuto" che faceva avanzare la pagina da sola.
// Accetto il tasto solo se risulta premuto stabilmente per >= ms (filtra i glitch).
bool keyPressed(Btn& k, bool now, uint32_t ms){
  if(now != k.raw){ k.raw=now; k.t=millis(); }                 // cambio lettura: riavvia il timer
  if(k.state != k.raw && millis()-k.t >= ms){ k.state=k.raw; return k.state; } // confermato
  return false;                                                // true solo alla pressione confermata
}

// ---- BOLLA / INCLINOMETRO ----
int   levelMode = 0;        // 0 = bolla (piano), 1 = inclinometro (verticale)
bool  angTared = false;     // azzeramento attivo
float angOffset = 0;        // offset di tare
float lastAng = 0;          // ultimo angolo misurato (per il tare)
float sAx = 0, sAy = 0, sAz = 1;  // accelerazioni filtrate (smoothing)

// ---- DISEGNO ----
uint16_t penCols[] = { TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_BLACK };
const int NPEN = 6;
int penIdx = 0, penR = 2;
int lastDX = -1, lastDY = -1;

// ---- WIFI ----
int wifiN = -2;            // -2 mai, -1 in corso
String wSSID[10]; int wRSSI[10];

// ---- CREDENZIALI WIFI (sovrascrivibili via BLE, vedi SETUP > WIFI) ----
// Di default uso le WIFI_SSID/WIFI_PASS definite in cima al file. Se pero' e'
// stata salvata una rete diversa via BLE (memoria non volatile, sopravvive al
// riavvio), uso quella. Cosi' si puo' cambiare rete senza mai ricompilare.
Preferences wifiPrefs;
String gWifiSsid = WIFI_SSID;
String gWifiPass = WIFI_PASS;
void wifiCredsLoad(){
  wifiPrefs.begin("wifi", true);   // sola lettura
  String s = wifiPrefs.getString("ssid", "");
  String p = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  if (s.length() > 0){ gWifiSsid = s; gWifiPass = p; }
}
void wifiCredsSave(const String& ssid, const String& pass){
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
  gWifiSsid = ssid; gWifiPass = pass;
}
void wifiCredsForget(){
  wifiPrefs.begin("wifi", false);
  wifiPrefs.clear();
  wifiPrefs.end();
  gWifiSsid = WIFI_SSID; gWifiPass = WIFI_PASS;
}

// ---- LoRa SX1262 ----
#define LORA_MOSI 21
#define LORA_MISO 22
#define LORA_SCK  20
#define LORA_IRQ  15
#define LORA_CS   23
#define LORA_BUSY 19
SX1262 radio = new Module(LORA_CS, LORA_IRQ, RADIOLIB_NC, LORA_BUSY);

struct Preset { const char* name; float freq; float bw; uint8_t sf; uint8_t cr; };
Preset PRESETS[] = {
  {"EU LongFast",   869.525, 250.0, 11, 5},
  {"EU 868.0 LF",   868.000, 250.0, 11, 5},
  {"EU MediumFast", 869.525, 250.0,  9, 5},
  {"EU ShortFast",  869.525, 250.0,  7, 5},
  {"EU LongSlow",   869.525, 125.0, 12, 8},
};
const int NUM_PRESETS = 5;
int presetIdx = 0;
float curFreq = 869.525;
const uint8_t MESH_SYNC = 0x2B; const uint16_t MESH_PRE = 16;
bool loraReady = false, loraOk = false;
volatile bool rxFlag = false;
uint32_t pktTot = 0, pktCrc = 0, lastFrom = 0;
long lastRssi = 0; float lastSnr = 0; uint8_t lastLen = 0;
// ultimo messaggio Meshtastic decodificato (solo canale con chiave di default)
String lastMsg = ""; uint32_t lastMsgFrom = 0, lastMsgTime = 0, pktMsg = 0;
#define MAX_NODES 30
uint32_t nodes[MAX_NODES]; int nodeCount = 0;

#if defined(ESP32)
ICACHE_RAM_ATTR
#endif
void setRxFlag(){ rxFlag = true; }

void applyPreset(int idx){
  Preset& p = PRESETS[idx]; curFreq = p.freq;
  int st = radio.begin(p.freq, p.bw, p.sf, p.cr, MESH_SYNC, 10, MESH_PRE, 3.0, true);
  loraOk = (st == RADIOLIB_ERR_NONE);
  Serial.printf("[LoRa] %s %.3f -> %d\n", p.name, p.freq, st);
  if (loraOk){ radio.setPacketReceivedAction(setRxFlag); radio.startReceive(); }
}
void loraInit(){
  if (loraReady) return;
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  auto& ioe = M5.getIOExpander(0);
  ioe.digitalWrite(7,false); delay(100); ioe.digitalWrite(7,true); delay(100);
  ioe.digitalWrite(5,true); ioe.digitalWrite(6,true);   // LNA + switch antenna
  applyPreset(presetIdx);
  loraReady = true;
}
void loraResetStats(){ pktTot=pktCrc=0; nodeCount=0; lastFrom=0; lastLen=0; lastRssi=0; lastSnr=0; lastMsg=""; lastMsgFrom=0; lastMsgTime=0; pktMsg=0; }
void loraAddNode(uint32_t id){ if(!id) return; for(int i=0;i<nodeCount;i++) if(nodes[i]==id) return; if(nodeCount<MAX_NODES) nodes[nodeCount++]=id; }
void nudgeFreq(float d){ curFreq += d; if(loraOk){ radio.setFrequency(curFreq); radio.startReceive(); } loraResetStats(); }

// --- Meshtastic: decifra col PSK di default e legge i messaggi di testo ---
// Chiave AES128 del canale "default" (psk = AQ==). Legge solo i canali con questa chiave.
const uint8_t MESH_KEY[16] = {0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};
bool meshDecrypt(const uint8_t* in, size_t len, uint32_t from, uint32_t pid, uint8_t* out){
  uint8_t nonce[16]; memset(nonce,0,16);
  memcpy(nonce,   &pid,  4);   // packet id (LE) nei primi 4 byte
  memcpy(nonce+8, &from, 4);   // node id del mittente (LE) nei byte 8..11
  mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
  if(mbedtls_aes_setkey_enc(&ctx, MESH_KEY, 128)!=0){ mbedtls_aes_free(&ctx); return false; }
  size_t nc_off=0; uint8_t stream[16]; memset(stream,0,16);
  int r = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, in, out);
  mbedtls_aes_free(&ctx);
  return r==0;
}
// estrae un eventuale testo dal protobuf Data (campo1=portnum, campo2=payload)
bool meshParseText(const uint8_t* d, size_t len, String& out){
  size_t i=0; uint8_t portnum=0xFF; String payload=""; bool havePayload=false;
  while(i<len){
    uint8_t tag=d[i++], field=tag>>3, wt=tag&7;
    if(wt==0){ uint64_t v=0; int s=0; while(i<len){ uint8_t b=d[i++]; v|=(uint64_t)(b&0x7f)<<s; s+=7; if(!(b&0x80))break; } if(field==1) portnum=(uint8_t)v; }
    else if(wt==2){ uint32_t l=0; int s=0; while(i<len){ uint8_t b=d[i++]; l|=(uint32_t)(b&0x7f)<<s; s+=7; if(!(b&0x80))break; } if(i+l>len) break; if(field==2){ payload=""; for(uint32_t k=0;k<l && k<160;k++) payload+=(char)d[i+k]; havePayload=true; } i+=l; }
    else if(wt==5) i+=4; else if(wt==1) i+=8; else break;
  }
  if(portnum==1 && havePayload){ out=payload; return true; }   // 1 = TEXT_MESSAGE_APP
  return false;
}
void handleLoraRx(){
  if(!loraReady || !rxFlag) return;
  rxFlag=false;
  uint8_t buf[256]; size_t len=radio.getPacketLength(); if(len>sizeof(buf)) len=sizeof(buf);
  int st=radio.readData(buf,len);
  if(st==RADIOLIB_ERR_NONE){
    pktTot++; lastLen=len; lastRssi=(long)radio.getRSSI(); lastSnr=radio.getSNR();
    uint32_t from=0,pid=0;
    if(len>=16){
      from=buf[4]|(buf[5]<<8)|(buf[6]<<16)|((uint32_t)buf[7]<<24); lastFrom=from; loraAddNode(from);
      pid =buf[8]|(buf[9]<<8)|(buf[10]<<16)|((uint32_t)buf[11]<<24);
      size_t plen=len-16;
      if(plen>0){
        uint8_t dec[240]; if(plen>sizeof(dec)) plen=sizeof(dec);
        if(meshDecrypt(buf+16, plen, from, pid, dec)){
          String txt; if(meshParseText(dec, plen, txt)){ lastMsg=txt; lastMsgFrom=from; lastMsgTime=millis(); pktMsg++; M5.Speaker.tone(2600,40); }
        }
      }
    }
    Serial.printf("[LoRa] len=%u RSSI=%ld from=!%08lx\n",(unsigned)len,lastRssi,(unsigned long)from);
  } else if(st==RADIOLIB_ERR_CRC_MISMATCH) pktCrc++;
}

// ---- IR (spegni/accendi TV multi-marca) ----
const uint16_t IR_TX = 9;        // LED IR onboard (GPIO9, datasheet Nesso N1)
IRsend irsend(IR_TX);
bool irReady = false;
int  irMode = 0;                 // 0 = spegni, 1 = accendi (KEY2 commuta)
const char* irState = "pronto"; uint32_t irMsg = 0;
void irInit(){
  if (irReady) return;
  irsend.begin(); irReady = true;   // l'IR e' sul GPIO9, non serve il 5V di Port.A
}
void irTvOff(){
  irsend.sendSAMSUNG(0xE0E040BF, 32);         delay(50);  // Samsung (power toggle)
  irsend.sendNEC(0x20DF10EF, 32);             delay(50);  // LG (power toggle)
  irsend.sendSony(0xA90, 12);                 delay(50);  // Sony 12bit
  irsend.sendSony(0xA90, 15);                 delay(50);  // Sony 15bit
  irsend.sendPanasonic64(0x40040100BCBD, 48); delay(50);  // Panasonic
  irsend.sendNEC(0x57E3E817, 32);             delay(50);  // Hisense/varie
}
void irTvOn(){
  irsend.sendSAMSUNG(0xE0E09966, 32);         delay(50);  // Samsung ON discreto
  irsend.sendNEC(0x20DF23DC, 32);             delay(50);  // LG ON discreto
  irsend.sendSony(0xA90, 12);                 delay(50);  // Sony (toggle)
  irsend.sendSony(0xA90, 15);                 delay(50);
  irsend.sendPanasonic64(0x40040100BCBD, 48); delay(50);  // Panasonic (toggle)
  irsend.sendNEC(0x57E3E817, 32);             delay(50);  // varie (toggle)
}

// ---- GPS (Quectel L80-M39, NMEA su UART) ----
// Collegato all'header di espansione a 8 pin in alto: GPIO07(D1)=RX ESP32,
// GPIO02(D2)=TX ESP32, alimentazione da +3V3 OUT / GND. Uso una UART hardware
// dedicata (Serial1) cosi' non tocco la Serial USB di debug.
#define GPS_RX_PIN 7   // ESP32 RX <- TX del modulo GPS
#define GPS_TX_PIN 2   // ESP32 TX -> RX del modulo GPS
#define GPS_BAUD   9600
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;
// Il fix-type (1=nessuno, 2=2D, 3=3D) e PDOP/VDOP vengono dalla frase GSA, che
// TinyGPS++ non decodifica di default -> la leggo "a mano" con TinyGPSCustom.
// Registro sia il prefisso GP che GN perche' un modulo multi-GNSS (come
// l'L80-M39, GPS+GLONASS) puo' usare l'uno o l'altro a seconda della config.
TinyGPSCustom gsaFixP(gps, "GPGSA", 2),  gsaFixN(gps, "GNGSA", 2);
TinyGPSCustom gsaPdopP(gps, "GPGSA", 15), gsaPdopN(gps, "GNGSA", 15);
TinyGPSCustom gsaVdopP(gps, "GPGSA", 17), gsaVdopN(gps, "GNGSA", 17);
bool     gpsReady = false;
uint32_t gpsLastChar = 0;    // millis() dell'ultimo byte ricevuto -> "modulo collegato?"
uint32_t gpsCharCount = 0;
int      gpsView = 0;        // 0 = STATO (fix/sat/dop), 1 = POSIZIONE (lat/lon/alt/data)
const int GPS_VIEWS = 2;

void gpsInit(){
  if (gpsReady) return;
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  gpsReady = true;
}
void handleGpsRx(){
  if (!gpsReady) return;
  while (gpsSerial.available()){
    gps.encode(gpsSerial.read());
    gpsLastChar = millis(); gpsCharCount++;
  }
}
bool gpsModuleConnected(){ return gpsReady && gpsCharCount>0 && (millis()-gpsLastChar < 3000); }
int  gpsFixType(){   // 1=nessun fix, 2=2D, 3=3D (0 = sconosciuto/non ancora ricevuto)
  if (gsaFixN.isValid()) return atoi(gsaFixN.value());
  if (gsaFixP.isValid()) return atoi(gsaFixP.value());
  return gps.location.isValid() ? 2 : 0;   // stima grezza se la frase GSA non e' ancora arrivata
}
float gpsPdop(){ if (gsaPdopN.isValid()) return atof(gsaPdopN.value()); if (gsaPdopP.isValid()) return atof(gsaPdopP.value()); return -1; }
float gpsVdop(){ if (gsaVdopN.isValid()) return atof(gsaVdopN.value()); if (gsaVdopP.isValid()) return atof(gsaVdopP.value()); return -1; }

// sincronizza l'orologio dal GPS (utile senza WiFi/internet: l'unica alternativa
// finora era l'NTP). Il GPS da' l'ora in UTC -> per calcolare l'epoch corretto
// scambio temporaneamente il fuso a UTC puro, converto, e ripristino TZ_INFO
// (altrimenti mktime() interpreterebbe l'orario UTC come se fosse gia' locale).
const char* gpsSyncState = "";
bool gpsTimeSync(){
  if (!(gps.date.isValid() && gps.time.isValid())) { gpsSyncState = "nessun dato GPS valido"; return false; }
  if (gps.date.age() > 2000 || gps.time.age() > 2000) { gpsSyncState = "dato GPS non aggiornato"; return false; }
  struct tm utcTm = {};
  utcTm.tm_year = gps.date.year() - 1900;
  utcTm.tm_mon  = gps.date.month() - 1;
  utcTm.tm_mday = gps.date.day();
  utcTm.tm_hour = gps.time.hour();
  utcTm.tm_min  = gps.time.minute();
  utcTm.tm_sec  = gps.time.second();
  utcTm.tm_isdst = 0;
  setenv("TZ", "UTC0", 1); tzset();
  time_t utcEpoch = mktime(&utcTm);
  setenv("TZ", TZ_INFO, 1); tzset();
  if (utcEpoch <= 0) { gpsSyncState = "errore conversione"; return false; }
  struct timeval tv; tv.tv_sec = utcEpoch; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  gpsSyncState = "ora sincronizzata!";
  return true;
}

// ---- METEO (OpenWeatherMap, richiede WIFI + un fix GPS) ----
// Uso la posizione del GPS (pagina GPS) per chiedere a OpenWeatherMap sia il
// meteo attuale sia le previsioni delle prossime ore (endpoint "forecast",
// step di 3h) via HTTPS. "setInsecure()" salta la verifica del certificato del
// server: evita di dover incorporare il certificato radice nel firmware,
// compromesso accettabile per un servizio meteo hobbistico (non e' un dato
// sensibile). L'aggiornamento e' manuale (KEY2): connessione WiFi + le due
// richieste impiegano qualche secondo, non ha senso rifarle di continuo.
enum { WX_CLEAR, WX_CLOUDS, WX_RAIN, WX_STORM, WX_SNOW, WX_FOG, WX_OTHER };
bool   wxBusy     = false;
String wxState    = "KEY2 = aggiorna";
bool   wxHaveData = false;
int    wxCat = WX_OTHER;
float  wxTemp=0, wxFeels=0, wxHum=0, wxPress=0, wxWindSpd=0;
String wxDesc = "";
uint32_t wxUpdated = 0;   // millis() dell'ultimo aggiornamento riuscito
int    wxView = 0;        // 0 = ATTUALE (con icona animata), 1 = PROSSIME ORE
const int WX_FC_N = 5;
uint32_t wxFcTime[WX_FC_N];
float    wxFcTemp[WX_FC_N];
int      wxFcCat[WX_FC_N];
int      wxFcCount = 0;

int wxCatFromMain(const String& m){
  if (m=="Clear") return WX_CLEAR;
  if (m=="Clouds") return WX_CLOUDS;
  if (m=="Rain" || m=="Drizzle") return WX_RAIN;
  if (m=="Thunderstorm") return WX_STORM;
  if (m=="Snow") return WX_SNOW;
  if (m=="Mist"||m=="Fog"||m=="Haze"||m=="Smoke"||m=="Dust"||m=="Sand") return WX_FOG;
  return WX_OTHER;
}
// icona meteo animata: nessuna immagine, solo primitive disegnate a mano e
// animate con millis(), cosi' funziona anche a schermo piccolo senza sprite esterni.
void drawWxIcon(int cx, int cy, int r, int cat, uint32_t t){
  float ph = (t % 3000) / 3000.0f;   // fase 0..1 ogni 3s, ciclica
  switch (cat){
    case WX_CLEAR: {
      canvas.fillCircle(cx, cy, (int)(r*0.55f), TFT_YELLOW);
      for (int i=0;i<8;i++){
        float a = ph*6.283f + i*0.785f;
        int x0=cx+(int)(cosf(a)*r*0.7f),  y0=cy+(int)(sinf(a)*r*0.7f);
        int x1=cx+(int)(cosf(a)*r*1.05f), y1=cy+(int)(sinf(a)*r*1.05f);
        canvas.drawLine(x0,y0,x1,y1,TFT_YELLOW);
      }
      break;
    }
    case WX_CLOUDS: {
      int dx = (int)(sinf(ph*6.283f)*4);
      canvas.fillCircle(cx-(int)(r*0.3f)+dx, cy,            (int)(r*0.45f), TFT_WHITE);
      canvas.fillCircle(cx+(int)(r*0.25f)+dx, cy-(int)(r*0.15f), (int)(r*0.5f),  TFT_WHITE);
      canvas.fillCircle(cx+(int)(r*0.05f)+dx, cy+(int)(r*0.15f), (int)(r*0.4f),  0xC618);
      break;
    }
    case WX_RAIN: {
      canvas.fillCircle(cx-(int)(r*0.2f), cy-(int)(r*0.2f), (int)(r*0.45f), TFT_WHITE);
      canvas.fillCircle(cx+(int)(r*0.25f), cy-(int)(r*0.1f), (int)(r*0.4f), 0xC618);
      for (int i=0;i<4;i++){
        float off = fmodf(ph*2.0f + i*0.25f, 1.0f);
        int xx = cx - (int)(r*0.6f) + i*(int)(r*0.4f);
        int y0 = cy + (int)(r*0.3f) + (int)(off*r*0.7f);
        canvas.drawLine(xx, y0, xx-2, y0+6, TFT_CYAN);
      }
      break;
    }
    case WX_STORM: {
      canvas.fillCircle(cx, cy-(int)(r*0.15f), (int)(r*0.5f), 0x8410);
      if (fmodf(ph*4.0f,1.0f) < 0.15f)
        canvas.fillTriangle(cx-4, cy, cx+6, cy, cx-2, cy+(int)(r*0.7f), TFT_YELLOW);
      break;
    }
    case WX_SNOW: {
      canvas.fillCircle(cx-(int)(r*0.2f), cy-(int)(r*0.2f), (int)(r*0.4f), 0xC618);
      for (int i=0;i<5;i++){
        float off = fmodf(ph + i*0.2f, 1.0f);
        int xx = cx - (int)(r*0.7f) + i*(int)(r*0.35f);
        int yy = cy - (int)(r*0.3f) + (int)(off*r*1.3f);
        canvas.drawLine(xx-2,yy,xx+2,yy,TFT_WHITE);
        canvas.drawLine(xx,yy-2,xx,yy+2,TFT_WHITE);
      }
      break;
    }
    case WX_FOG: {
      for (int i=0;i<3;i++){
        int yy = cy - (int)(r*0.3f) + i*(int)(r*0.3f);
        int off = (int)(sinf(ph*6.283f+i)*4);
        canvas.drawLine(cx-(int)(r*0.7f)+off, yy, cx+(int)(r*0.7f)+off, yy, COL_MUTED);
      }
      break;
    }
    default: canvas.drawCircle(cx, cy, (int)(r*0.5f), TFT_WHITE); break;
  }
}

bool weatherFetch(){
  if (!gps.location.isValid()){ wxState = "serve un fix GPS"; return false; }
  double lat = gps.location.lat(), lon = gps.location.lng();

  wxBusy = true; wxState = "connessione WiFi..."; render();
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF); delay(150);
  WiFi.mode(WIFI_STA); delay(150);
  WiFi.begin(gWifiSsid.c_str(), gWifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0<15000) delay(200);
  if (WiFi.status()!=WL_CONNECTED){
    wxState = "WiFi fallito"; WiFi.mode(WIFI_OFF); wxBusy=false; return false;
  }

  bool ok = false;
  WiFiClientSecure client;
  client.setInsecure();

  // --- meteo attuale ---
  wxState = "richiesta meteo..."; render();
  {
    HTTPClient http;
    char url[220];
    snprintf(url, sizeof(url),
      "https://api.openweathermap.org/data/2.5/weather?lat=%.6f&lon=%.6f&appid=%s&units=metric&lang=it",
      lat, lon, OWM_API_KEY);
    if (http.begin(client, url)){
      int code = http.GET();
      if (code == 200){
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)){
          wxTemp    = doc["main"]["temp"]       | 0.0;
          wxFeels   = doc["main"]["feels_like"] | 0.0;
          wxHum     = doc["main"]["humidity"]   | 0.0;
          wxPress   = doc["main"]["pressure"]   | 0.0;
          wxWindSpd = (doc["wind"]["speed"] | 0.0) * 3.6f;   // m/s -> km/h
          wxDesc    = String((const char*)(doc["weather"][0]["description"] | ""));
          wxCat     = wxCatFromMain(String((const char*)(doc["weather"][0]["main"] | "")));
          wxHaveData = true; wxUpdated = millis();
          ok = true;
        }
      }
      http.end();
    }
  }

  // --- previsioni prossime ore (step 3h, "cnt" limita la risposta lato server) ---
  if (ok){
    wxState = "previsioni..."; render();
    HTTPClient http2;
    char url2[220];
    snprintf(url2, sizeof(url2),
      "https://api.openweathermap.org/data/2.5/forecast?lat=%.6f&lon=%.6f&appid=%s&units=metric&lang=it&cnt=%d",
      lat, lon, OWM_API_KEY, WX_FC_N);
    if (http2.begin(client, url2)){
      int code2 = http2.GET();
      if (code2 == 200){
        String payload2 = http2.getString();
        JsonDocument doc2;
        if (!deserializeJson(doc2, payload2)){
          JsonArray list = doc2["list"].as<JsonArray>();
          wxFcCount = 0;
          for (JsonObject e : list){
            if (wxFcCount >= WX_FC_N) break;
            wxFcTime[wxFcCount] = e["dt"] | 0;
            wxFcTemp[wxFcCount] = e["main"]["temp"] | 0.0;
            wxFcCat[wxFcCount]  = wxCatFromMain(String((const char*)(e["weather"][0]["main"] | "")));
            wxFcCount++;
          }
        }
      }
      http2.end();
    }
  }

  wxState = ok ? "aggiornato!" : "errore richiesta";
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  wxBusy = false;
  return ok;
}

// ---- BLE (stack condiviso: air-mouse HID + configurazione WiFi) ----
// Un solo radio BLE, un solo NimBLEDevice::init()/server: sia il mouse HID sia
// il servizio di configurazione WiFi (piu' sotto) vivono sullo stesso server,
// aggiunti come servizi separati la prima volta che la relativa pagina/modalita'
// viene usata. Il nome del dispositivo e' generico ("Nesso N1") perche' ora
// serve entrambi gli scopi.
bool bleConnected  = false;   // true se un centrale (PC/telefono) e' connesso via BLE
bool bleStackReady = false;
NimBLEServer* gBleServer = nullptr;
// dichiarati qui (non piu' giu' nella sezione AIR MOUSE) perche' servono anche
// a bleAdvertiseStart(), definita prima di quella sezione nel file.
NimBLEServer*         mBleServer   = nullptr;
NimBLEHIDDevice*      mHid         = nullptr;
NimBLECharacteristic* mInputMouse  = nullptr;
bool mouseInited = false;
class SharedBleServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    // di default l'intervallo di connessione negoziato puo' essere lento
    // (anche 30-50 ms) -> per il mouse i movimenti arriverebbero "a raffica"
    // invece che fluidi. Chiedo un intervallo corto (7.5-15 ms) per chiunque si connetta.
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false; NimBLEDevice::startAdvertising();
  }
};
void bleStackInit(){
  if (bleStackReady) return;
  NimBLEDevice::init("Nesso N1");
  gBleServer = NimBLEDevice::createServer();
  gBleServer->setCallbacks(new SharedBleServerCB());
  bleStackReady = true;
}

// ---- CONFIGURAZIONE WIFI VIA BLE ----
// Da telefono, con una app BLE generica (nRF Connect su Android, LightBlue su
// iOS): connettiti al dispositivo "Nesso N1", apri il servizio di config WiFi,
// scrivi il nome della rete nella caratteristica SSID (testo semplice) e la
// password in quella PASSWORD. Scrivere la PASSWORD e' anche il "conferma e
// salva": da quel momento il Nesso N1 usa quella rete (salvata in memoria non
// volatile, sopravvive al riavvio) al posto di quella scritta nel codice.
#define WIFICFG_SVC_UUID  "6f6a1000-b5a3-4393-a0a9-e50e24dcca9e"
#define WIFICFG_SSID_UUID "6f6a1001-b5a3-4393-a0a9-e50e24dcca9e"
#define WIFICFG_PASS_UUID "6f6a1002-b5a3-4393-a0a9-e50e24dcca9e"
#define WIFICFG_STAT_UUID "6f6a1003-b5a3-4393-a0a9-e50e24dcca9e"
NimBLECharacteristic* wcfgSsidChr = nullptr;
NimBLECharacteristic* wcfgPassChr = nullptr;
NimBLECharacteristic* wcfgStatChr = nullptr;
bool   wcfgReady = false;
String wcfgPendingSsid = "";
class WifiCfgCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    if (c == wcfgSsidChr){
      wcfgPendingSsid = String(v.c_str());
    } else if (c == wcfgPassChr){
      String pass = String(v.c_str());
      if (wcfgPendingSsid.length() > 0){
        wifiCredsSave(wcfgPendingSsid, pass);
        std::string msg = std::string("salvato: ") + wcfgPendingSsid.c_str();
        wcfgStatChr->setValue(msg);
        wcfgStatChr->notify();
      }
    }
  }
};
void wifiCfgInit(){
  if (wcfgReady) return;
  bleStackInit();
  NimBLEService* svc = gBleServer->createService(WIFICFG_SVC_UUID);
  wcfgSsidChr = svc->createCharacteristic(WIFICFG_SSID_UUID, NIMBLE_PROPERTY::WRITE);
  wcfgPassChr = svc->createCharacteristic(WIFICFG_PASS_UUID, NIMBLE_PROPERTY::WRITE);
  wcfgStatChr = svc->createCharacteristic(WIFICFG_STAT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  static WifiCfgCB cb;
  wcfgSsidChr->setCallbacks(&cb);
  wcfgPassChr->setCallbacks(&cb);
  wcfgStatChr->setValue(std::string("in attesa"));
  // NB: niente svc->start()/server->start() qui, ne' l'UUID di questo servizio
  // nell'advertising: vedi bleAdvertiseStart() per il perche'.
  wcfgReady = true;
}
void bleAdvertiseStart(){
  // Configurazione di sicurezza + avvio del server/advertising fatti UNA SOLA
  // VOLTA, qui, dopo che tutti i servizi GATT (HID mouse + config WiFi) sono
  // gia' stati creati da mouseInit()/wifiCfgInit(). Il profilo HID-over-GATT
  // richiede il bonding: Windows lo accetta anche senza, ma Android in pratica
  // ignora/nasconde i dispositivi HID BLE che non lo supportano. "Just Works"
  // (nessun PIN da digitare, bonding si' / MITM e secure-connections no) e' la
  // configurazione minima compatibile con entrambi.
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(HID_MOUSE);
  if (mHid) adv->addServiceUUID(mHid->getHidService()->getUUID());
  // niente UUID del servizio wifi-cfg in advertising: il pacchetto BLE
  // "legacy" ha un limite di 31 byte, gia' quasi pieno con nome + UUID HID, e
  // un secondo UUID a 128 bit rischia di farlo sforare (alcuni scanner, Android
  // in primis, ignorano pacchetti malformati). Non serve comunque: una volta
  // connesso, qualunque app BLE scopre tutti i servizi via GATT discovery.
  adv->enableScanResponse(true);
  // NimBLEDevice::init(nome) imposta il nome SOLO nella caratteristica GAP
  // (leggibile dopo la connessione) -> senza questa riga il pacchetto di
  // advertising/scan-response non contiene il nome, ed e' esattamente per
  // questo che appariva "senza nome" negli scanner (nRF Connect, Android).
  adv->setName("Nesso N1");
  gBleServer->start();
  adv->start();
}

// ---- AIR MOUSE (BLE HID, implementato su NimBLE-Arduino) ----
// Il Nesso N1 si presenta a Windows come un mouse Bluetooth LE (HID over GATT).
// Da Windows: Impostazioni > Bluetooth e dispositivi > Aggiungi dispositivo,
// scegli "Nesso N1" (nome del dispositivo BLE, condiviso anche dalla config
// WiFi). Il cursore segue la VELOCITA' di rotazione del
// polso (giroscopio, come i telecomandi "air mouse" veri), non l'inclinazione
// assoluta: niente drift, e non serve tenerlo in una posa precisa. La versione
// con l'accelerometro (tilt assoluta) era instabile perche' il rumore del
// sensore finiva direttamente sul cursore ad ogni giro di loop() senza filtro
// ne' limite di frequenza -> ora c'e' un filtro passa-basso sul giroscopio,
// una zona morta, una calibrazione del bias a riposo e un invio a ~80 Hz fisso.
// Report HID standard a 4 byte: [bottoni, X relativo, Y relativo, rotellina].
static const uint8_t MOUSE_REPORT_MAP[] = {
  0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x01,
  0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03,
    0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x03,
    0x05,0x01,
    0x09,0x30, 0x09,0x31, 0x09,0x38,
    0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06,
  0xC0,
  0xC0,
  // tastiera (report 2): usata dalla pagina COMPANION per installare l'app sul PC
  // [modificatori, riservato, 6 tasti] + report di uscita coi LED (serve Bloc Num)
  0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x02,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x05,0x08, 0x19,0x01, 0x29,0x05, 0x95,0x05, 0x75,0x01, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x05,0x07, 0x19,0x00, 0x29,0x65, 0x15,0x00, 0x25,0x65, 0x95,0x06, 0x75,0x08, 0x81,0x00,
  0xC0
};
NimBLECharacteristic* mInputKb  = nullptr;   // tasti premuti (report 2)
NimBLECharacteristic* mOutputKb = nullptr;   // LED dal PC (Bloc Num, Maiusc...)
volatile uint8_t kbLeds = 0; volatile bool kbLedKnown = false;
class KbLedCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    if(!v.empty()){ kbLeds = (uint8_t)v[0]; kbLedKnown = true; }
  }
};
bool  mouseHold      = false;      // tocco = blocca il puntatore (per riposizionare il polso)
// VERO JOYSTICK: posizione = direzione + velocita', torna sempre al centro da
// solo quando rilasci (nessuna integrazione -> nessuna deriva possibile, e'
// un valore ASSOLUTO ricalcolato ad ogni campione). La versione con la BOLLA si
// rompeva perche' l'angolo veniva letto con atan2 su assi FISSI dello schermo,
// che vanno in singolarita' se il dispositivo e' impugnato come un puntatore
// (un asse si avvicina a zero). Qui invece il tilt e' la PROIEZIONE del vettore
// di gravita' corrente su un piano perpendicolare al riferimento calibrato (la
// posa con cui tieni il dispositivo quando entri in pagina/sblocchi) -> nessuna
// singolarita' in nessuna posa, e nessuna deriva perche' non si integra nulla.
float mRefX=0, mRefY=0, mRefZ=1;     // vettore di gravita' calibrato (posa "centro")
float mTanXx=1, mTanXy=0, mTanXz=0;  // base tangente "X schermo" (perpendicolare al riferimento)
float mTanYx=0, mTanYy=1, mTanYz=0;  // base tangente "Y schermo"
float sAX=0, sAY=0;                  // proiezione filtrata (g), usata anche per il pallino in UI
float mAccX = 0, mAccY = 0;          // resto frazionario per movimento fluido anche a bassa velocita'
uint32_t mLastUpdate = 0;
const float ACC_FILTER    = 0.35f;   // filtro passa-basso leggero (solo rumore: essendo assoluto non serve stabilizzare la deriva)
const float MOUSE_DEAD    = 0.025f;  // g, zona morta al centro
const float MOUSE_MAXTILT = 0.22f;   // g, tilt per la velocita' massima: basta poca inclinazione
const float MOUSE_SENS    = 500.0f;  // px/s alla massima inclinazione
const uint32_t MOUSE_UPDATE_MS = 12; // ~80 Hz, come un mouse USB tipico

void mouseInit(){
  if (mouseInited) return;
  bleStackInit();
  mBleServer = gBleServer;

  mHid = new NimBLEHIDDevice(mBleServer);
  mHid->setManufacturer("M5Stack");
  mHid->setPnp(0x02, 0xE502, 0xA111, 0x0100);
  mHid->setHidInfo(0x00, 0x01);
  mHid->setReportMap((uint8_t*)MOUSE_REPORT_MAP, sizeof(MOUSE_REPORT_MAP));
  mInputMouse = mHid->getInputReport(1);
  mInputKb    = mHid->getInputReport(2);
  mOutputKb   = mHid->getOutputReport(2);
  static KbLedCB kbLedCb;
  mOutputKb->setCallbacks(&kbLedCb);
  mHid->setBatteryLevel(100);
  // NB: niente setSecurityAuth/advertising/server->start() qui: li faccio una
  // volta sola, in bleAdvertiseStart(), DOPO che tutti i servizi (HID + wifi-
  // cfg) sono stati creati. La libreria stessa lo richiede (vedi il commento
  // "no-op now, services started by server start" in NimBLEHIDDevice::startServices):
  // avviare il server/advertising piu' volte con servizi aggiunti in mezzo, come
  // facevo prima, puo' produrre un pacchetto BLE instabile che alcuni scanner
  // (Android, nRF Connect) scartano anche se Windows lo tollera.
  mouseInited = true;
}
void mouseCalibrate(){
  // media di alcuni campioni da fermo per prendere il vettore di gravita' della
  // posa attuale come "centro" del joystick.
  float sx=0, sy=0, sz=0; const int N=20;
  for(int i=0;i<N;i++){
    float ax=0,ay=0,az=0; M5.Imu.update(); M5.Imu.getAccelData(&ax,&ay,&az);
    sx += ax; sy += ay; sz += az; delay(4);
  }
  float rx=sx/N, ry=sy/N, rz=sz/N;
  float mag = sqrtf(rx*rx+ry*ry+rz*rz); if (mag < 0.1f) mag = 1.0f;
  mRefX = rx/mag; mRefY = ry/mag; mRefZ = rz/mag;

  // costruisco una base tangente ("X schermo"/"Y schermo") perpendicolare al
  // riferimento: qualunque sia la posa, questa base non degenera mai (a meno
  // di non tenere il dispositivo esattamente verticale, gestito col fallback).
  float upx=0, upy=0, upz=1;
  if (fabsf(mRefZ) > 0.9f) { upx=0; upy=1; upz=0; }
  float tx = mRefY*upz - mRefZ*upy;
  float ty = mRefZ*upx - mRefX*upz;
  float tz = mRefX*upy - mRefY*upx;
  float tl = sqrtf(tx*tx+ty*ty+tz*tz); if (tl < 1e-4f) tl = 1.0f;
  mTanXx = tx/tl; mTanXy = ty/tl; mTanXz = tz/tl;
  mTanYx = mRefY*mTanXz - mRefZ*mTanXy;
  mTanYy = mRefZ*mTanXx - mRefX*mTanXz;
  mTanYz = mRefX*mTanXy - mRefY*mTanXx;

  sAX = sAY = mAccX = mAccY = 0;
  mLastUpdate = millis();
}
void mouseSend(uint8_t btn, int8_t dx, int8_t dy, int8_t wheel){
  uint8_t rpt[4] = { btn, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
  mInputMouse->setValue(rpt, sizeof(rpt));
  mInputMouse->notify();
}
void mouseUpdate(){
  if (!mouseInited || !bleConnected || mouseHold) return;
  uint32_t now = millis();
  if (now - mLastUpdate < MOUSE_UPDATE_MS) return;   // invio a frequenza fissa, non ad ogni loop()
  float dt = (now - mLastUpdate) / 1000.0f;
  if (dt > 0.1f) dt = MOUSE_UPDATE_MS / 1000.0f;      // dopo una pausa lunga non fare uno scatto
  mLastUpdate = now;

  M5.Imu.update();
  float ax=0,ay=0,az=0; M5.Imu.getAccelData(&ax,&ay,&az);

  // proiezione del vettore di gravita' corrente sul piano tangente calibrato:
  // e' un valore ASSOLUTO (non integrato) -> torna sempre a zero da solo quando
  // il polso torna alla posa di calibrazione, come una levetta con la molla.
  float px = ax*mTanXx + ay*mTanXy + az*mTanXz;
  float py = ax*mTanYx + ay*mTanYy + az*mTanYz;
  sAX = sAX*(1.0f-ACC_FILTER) + px*ACC_FILTER;
  sAY = sAY*(1.0f-ACC_FILTER) + py*ACC_FILTER;

  float dist = sqrtf(sAX*sAX + sAY*sAY);
  if (dist >= MOUSE_DEAD){
    // direzione dalla proiezione, velocita' dalla distanza dal centro (curva
    // quadratica: fine vicino al centro, veloce vicino al bordo)
    float norm  = constrain((dist-MOUSE_DEAD) / (MOUSE_MAXTILT-MOUSE_DEAD), 0.0f, 1.0f);
    float speed = norm*norm * MOUSE_SENS;
    float ux = sAX/dist, uy = sAY/dist;
    mAccX +=  ux*speed*dt;
    mAccY += -uy*speed*dt;
  }
  int mx = (int)mAccX, my = (int)mAccY;
  mAccX -= mx; mAccY -= my;
  mx = constrain(mx, -100, 100); my = constrain(my, -100, 100);
  if (mx != 0 || my != 0) mouseSend(0, mx, my, 0);
}
void mouseClick(){
  if (!mouseInited || !bleConnected) return;
  mouseSend(1, 0, 0, 0); delay(30); mouseSend(0, 0, 0, 0);   // click sinistro: premi + rilascia
}

// ---- TEMPO ----
void shiftTime(long secs){ time_t t=time(nullptr)+secs; struct timeval tv; tv.tv_sec=t; tv.tv_usec=0; settimeofday(&tv,nullptr); }
String clockStr(){ struct tm ti; if(!getLocalTime(&ti,5)) return "--:--"; char b[8]; strftime(b,8,"%H:%M",&ti); return String(b); }
String clockStrSec(){ struct tm ti; if(!getLocalTime(&ti,5)) return "--:--:--"; char b[12]; strftime(b,12,"%H:%M:%S",&ti); return String(b); }
const char* ntpState = "premi NTP";
bool ntpSync(){
  ntpState = "connessione..."; renderBarOnly();
  // reset pulito della radio: dopo un WIFI_OFF (scan/sync precedente) il solo
  // WIFI_STA a volte non aggancia su ESP32-C6. Ciclo OFF->STA con piccole pause.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF); delay(150);
  WiFi.mode(WIFI_STA); delay(150);
  WiFi.begin(gWifiSsid.c_str(), gWifiPass.c_str());
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<15000) delay(200);
  if(WiFi.status()!=WL_CONNECTED){
    Serial.printf("[WiFi] connect fallito, status=%d\n", WiFi.status());
    ntpState="WiFi fallito"; WiFi.mode(WIFI_OFF); return false;
  }
  Serial.printf("[WiFi] connesso: %s\n", WiFi.localIP().toString().c_str());
  ntpState = "sync NTP..."; renderBarOnly();
  // l'orologio e' gia' preimpostato (2024), quindi getLocalTime tornerebbe "valido"
  // all'istante: azzero l'ora cosi' attende il vero aggiornamento NTP prima che io
  // spenga il WiFi. Se va in timeout, ripristino l'ora precedente.
  time_t before = time(nullptr); uint32_t ms0 = millis();
  struct timeval z; z.tv_sec=0; z.tv_usec=0; settimeofday(&z, nullptr);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  struct tm ti; bool ok = getLocalTime(&ti, 8000);   // attende l'ora reale dalla rete
  if(!ok){ struct timeval r; r.tv_sec=before+(millis()-ms0)/1000; r.tv_usec=0; settimeofday(&r,nullptr); }
  ntpState = ok ? "ora aggiornata!" : "NTP timeout";
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);   // spengo la radio dopo la sync
  return ok;
}

// ---- stato UI: feedback SYNC, risparmio, torcia, cronometro ----
bool ntpBusy = false;                  // SYNC in corso -> pulsante evidenziato
int  setupMode = 0;                    // 0 = ora, 1 = batteria, 2 = risparmio, 3 = wifi
const int NUM_SETUP = 4;
// risparmio: solo timeout schermo. NB: il backlight del Nesso N1 e' on/off
// (pin IO-expander E1.P2, niente PWM) quindi NON e' dimmerabile via software.
const uint32_t SCR_TIMEOUTS[] = {0, 15000, 30000, 60000, 120000, 300000};
const char*    SCR_TLABEL[]   = {"Off", "15 s", "30 s", "1 m", "2 m", "5 m"};
const int NUM_SCR = 6; int scrIdx = 3;          // default: schermo off dopo 1 min
bool screenOn = true; uint32_t lastActivity = 0;
// torcia: il backlight non si dima, quindi l'intensita' e' resa con la tonalita'
// del riempimento (piu' chiaro = passa piu' luce attraverso l'LCD).
const uint16_t TORCH_SHADE[] = {0x4208, 0x8410, 0xC618, 0xFFFF};
const char*    TORCH_LBL[]   = {"25%", "50%", "75%", "MAX"};
const int TORCH_N = 4; int torchLevel = 3;
bool cronoRun = false; uint32_t cronoStart = 0, cronoAcc = 0;
// gioco biglia (inclinazione tramite BMI270)
float ballX=0, ballY=0, ballVX=0, ballVY=0;
int   tgtX=0, tgtY=0, gScore=0; bool gInit=false;
void ntpSyncUI(){ ntpBusy=true; render(); ntpSync(); ntpBusy=false; }

// ================= GRAFICA =====================================
void drawBatt(int x,int y,int w,int h){
  canvas.drawRoundRect(x,y,w,h,2,TFT_WHITE);
  canvas.fillRect(x+w,y+h/3,2,h/3,TFT_WHITE);
  uint16_t c = gLevel>50?TFT_GREEN:(gLevel>20?TFT_ORANGE:TFT_RED);
  if(gLevel>=0){ int f=constrain(gLevel,0,100)*(w-3)/100; canvas.fillRect(x+2,y+2,f,h-4,c); }
  if(gChg){ canvas.drawLine(x+w/2,y+2,x+w/2-3,y+h/2,TFT_YELLOW); canvas.drawLine(x+w/2-3,y+h/2,x+w/2+3,y+h/2,TFT_YELLOW); canvas.drawLine(x+w/2+3,y+h/2,x+w/2,y+h-2,TFT_YELLOW); }
}
void statusBar(){
  canvas.fillRect(0,0,W,BAR_H,COL_BAR);
  canvas.setTextFont(&fonts::Font0);
  // ora a sinistra
  canvas.setTextColor(TFT_WHITE,COL_BAR);
  canvas.setTextDatum(middle_left);
  canvas.drawString(clockStr().c_str(), 4, BAR_H/2-1);
  // titolo della pagina, centrato
  canvas.setTextColor(COL_ACCENT,COL_BAR);
  canvas.setTextDatum(middle_center);
  canvas.drawString(TITLE[page], W/2, BAR_H/2-1);
  // batteria a destra: solo icona (la % esatta e' nella pagina SETUP)
  drawBatt(W-26,5,20,12);
  // indicatore di pagina: barra segmentata sul bordo basso della status bar
  int segW = W/PG_COUNT;
  for(int i=0;i<PG_COUNT;i++)
    canvas.fillRect(i*segW+1, BAR_H-2, segW-2, 2, (i==page)?COL_ACCENT:COL_LINE);
  canvas.setTextDatum(top_left);
}
// barre di segnale (4 tacche) in base a rssi
void signalBars(int x,int y,int rssi){
  int n = rssi>-55?4:(rssi>-67?3:(rssi>-78?2:1));
  uint16_t c = n>=3?TFT_GREEN:(n==2?TFT_ORANGE:TFT_RED);
  for(int i=0;i<4;i++){ int bh=4+i*3; canvas.drawRect(x+i*5,y+12-bh,3,bh,TFT_DARKGREY); if(i<n) canvas.fillRect(x+i*5,y+12-bh,3,bh,c); }
}
void drawBtnC(const TBtn& b, uint16_t col){
  canvas.fillRoundRect(b.x,b.y,b.w,b.h,5,col);
  canvas.drawRoundRect(b.x,b.y,b.w,b.h,5,TFT_WHITE);
  canvas.setTextColor(TFT_WHITE,col); canvas.setTextDatum(middle_center);
  canvas.setTextFont(&fonts::Font0); canvas.drawString(b.lbl,b.x+b.w/2,b.y+b.h/2);
  canvas.setTextDatum(top_left);
}
void drawBtn(const TBtn& b){ drawBtnC(b, b.col); }
void drawSeg(int x0,int y0,int x1,int y1,uint16_t c,int r){
  int dx=x1-x0,dy=y1-y0,s=max(abs(dx),abs(dy)); if(s<1)s=1;
  for(int i=0;i<=s;i++) canvas.fillCircle(x0+dx*i/s,y0+dy*i/s,r,c);
}

// ----- pulsanti delle pagine -----
TBtn loraMinus = {6, H-30, 36, 22, "-0.1", TFT_NAVY};
TBtn loraPlus  = {W-42, H-30, 36, 22, "+0.1", TFT_NAVY};
// IR: il cerchio "power" e' disegnato a mano al centro
const int IR_CX = W/2, IR_CY = BAR_H+70, IR_CR = 40;
// SETUP pagina 1 (orologio)
TBtn setHm = {8,  86, 26, 26, "H-", TFT_NAVY};
TBtn setHp = {38, 86, 26, 26, "H+", TFT_NAVY};
TBtn setMm = {72, 86, 26, 26, "M-", TFT_NAVY};
TBtn setMp = {102,86, 26, 26, "M+", TFT_NAVY};
TBtn setNtp= {18, 120, 100, 26, "SYNC NTP", TFT_DARKGREEN};
// SETUP pagina 3 (risparmio): selettore timeout schermo
TBtn scrMinus = {8,    96, 34, 30, "-", TFT_NAVY};
TBtn scrPlus  = {W-42, 96, 34, 30, "+", TFT_NAVY};

// ----- render per pagina -----
void renderDraw(){   // canvas e' persistente: ridisegno solo barra + tavolozza
  statusBar();
  int sw = W/NPEN;
  for(int i=0;i<NPEN;i++){
    canvas.fillRect(i*sw, H-PAL_H, sw, PAL_H, penCols[i]);
    if(penCols[i]==TFT_BLACK){ canvas.drawLine(i*sw+4,H-PAL_H+4,i*sw+sw-4,H-4,TFT_WHITE); } // gomma
    if(i==penIdx) canvas.drawRect(i*sw,H-PAL_H,sw,PAL_H,TFT_YELLOW);
    else          canvas.drawRect(i*sw,H-PAL_H,sw,PAL_H,0x4208);
  }
  canvas.pushSprite(0,0);
}
void renderLevel(){
  canvas.fillSprite(COL_BG); statusBar();
  float ax=0,ay=0,az=0; M5.Imu.update(); M5.Imu.getAccelData(&ax,&ay,&az);
  // smoothing (media esponenziale) per ridurre il tremolio
  sAx = sAx*0.82f + ax*0.18f; sAy = sAy*0.82f + ay*0.18f; sAz = sAz*0.82f + az*0.18f;
  ax = sAx; ay = sAy; az = sAz;

  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_CYAN,COL_BG);
  canvas.drawString(levelMode==0 ? "LIVELLA  (KEY2)" : "INCLINOMETRO (KEY2)", W/2, BAR_H+8);

  if(levelMode==0){
    // ---- bolla per piani orizzontali ----
    int cx=W/2, cy=BAR_H+96, R=56;
    canvas.fillCircle(cx,cy,R+3,0x0841);
    canvas.drawCircle(cx,cy,R,TFT_DARKGREY);
    canvas.drawCircle(cx,cy,R*2/3,0x3186);
    canvas.drawCircle(cx,cy,R/3,0x3186);
    canvas.drawLine(cx-R,cy,cx+R,cy,0x3186);
    canvas.drawLine(cx,cy-R,cx,cy+R,0x3186);
    int bx=cx+(int)(ay*R*1.6f), by=cy+(int)(ax*R*1.6f);
    bx=constrain(bx,cx-R,cx+R); by=constrain(by,cy-R,cy+R);
    float d=sqrtf((float)(bx-cx)*(bx-cx)+(by-cy)*(by-cy));
    uint16_t bc = d<8?TFT_GREEN:(d<R/3?TFT_CYAN:TFT_ORANGE);
    canvas.fillCircle(bx,by,11,bc); canvas.drawCircle(bx,by,11,TFT_WHITE);
    canvas.drawCircle(cx,cy,12,0x528A);
    float angX = atan2f(ax, az)*57.2958f;   // inclinazione asse corto
    float angY = atan2f(ay, az)*57.2958f;    // inclinazione asse lungo
    canvas.setTextColor(d<8?TFT_GREEN:TFT_WHITE, COL_BG);
    canvas.drawString(d<8?"IN BOLLA":"inclina", cx, cy+R+16);
    canvas.setTextColor(TFT_WHITE,COL_BG); char b[24];
    sprintf(b,"X %+.1f   Y %+.1f", angX, angY);
    canvas.drawString(b, cx, H-14);
  } else {
    // ---- inclinometro: angolo dell'asse lungo dall'orizzontale ----
    float ang = atan2f(ay, sqrtf(ax*ax+az*az))*57.2958f;  // 0 = orizz, 90 = verticale
    lastAng = ang;
    float shown = ang - (angTared?angOffset:0.0f);
    // numero grande
    canvas.setTextFont(&fonts::Font4);
    canvas.setTextColor(fabsf(shown)<0.5f?TFT_GREEN:TFT_WHITE, COL_BG);
    char nb[12]; sprintf(nb,"%.1f", shown);
    canvas.drawString(nb, W/2, BAR_H+34);
    canvas.setTextFont(&fonts::Font0); canvas.setTextColor(TFT_DARKGREY,COL_BG);
    canvas.drawString("gradi (90=verticale)", W/2, BAR_H+54);
    // goniometro (semicerchio verso l'alto, 0 = su)
    int cx=W/2, cy=BAR_H+150, R=56;
    canvas.drawLine(cx-R-2,cy,cx+R+2,cy,0x3186);
    for(int a=-90;a<=90;a+=15){
      float rad=a*0.017453f; int len=(a%45==0)?10:6;
      int x0=cx+(int)((R-len)*sinf(rad)), y0=cy-(int)((R-len)*cosf(rad));
      int x1=cx+(int)(R*sinf(rad)),       y1=cy-(int)(R*cosf(rad));
      canvas.drawLine(x0,y0,x1,y1,0x6B4D);
    }
    float rr=shown; if(rr>90)rr=90; if(rr<-90)rr=-90;
    float rad=rr*0.017453f;
    int ex=cx+(int)(R*sinf(rad)), ey=cy-(int)(R*cosf(rad));
    uint16_t nc = angTared?TFT_ORANGE:TFT_GREEN;
    canvas.drawLine(cx,cy,ex,ey,nc); canvas.drawLine(cx-1,cy,ex,ey,nc);
    canvas.fillCircle(cx,cy,4,TFT_WHITE);
    canvas.setTextColor(TFT_CYAN,COL_BG);
    canvas.drawString(angTared?"tocca: annulla zero":"tocca: azzera", W/2, H-12);
  }
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderWifi(){
  canvas.fillSprite(COL_BG); statusBar();
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(TFT_WHITE,COL_BG);
  canvas.setTextDatum(middle_center);
  if(wifiN==-2){ canvas.drawString("KEY2 = cerca reti",W/2,H/2); canvas.setTextDatum(top_left); canvas.pushSprite(0,0); return; }
  if(wifiN==-1){ canvas.drawString("scansione...",W/2,H/2); canvas.setTextDatum(top_left); canvas.pushSprite(0,0); return; }
  if(wifiN==0){ canvas.setTextColor(COL_MUTED,COL_BG); canvas.drawString("nessuna rete trovata",W/2,H/2); canvas.setTextDatum(top_left); canvas.pushSprite(0,0); return; }
  int show = wifiN<9?wifiN:9;
  for(int i=0;i<show;i++){
    int y=BAR_H+4+i*23;
    canvas.fillRoundRect(2,y,W-4,21,3,COL_PANEL);
    signalBars(6,y+5,wRSSI[i]);
    canvas.setTextColor(TFT_WHITE,COL_PANEL); canvas.setTextDatum(middle_left);
    canvas.drawString(wSSID[i].substring(0,11).c_str(), 30, y+10);
    char rb[8]; sprintf(rb,"%d",wRSSI[i]);     // RSSI in dBm a destra
    canvas.setTextColor(COL_MUTED,COL_PANEL); canvas.setTextDatum(middle_right);
    canvas.drawString(rb, W-8, y+10);
  }
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void drawPower(int cx,int cy,int r,uint16_t c){
  canvas.drawCircle(cx,cy,r,c);
  canvas.drawCircle(cx,cy,r-1,c);
  canvas.fillRect(cx-3,cy-r-1,6,4,TFT_RED);   // notch in alto (anello aperto)
  canvas.fillRect(cx-1,cy-r+1,3,r/2+3,c);     // barra verticale
}
void renderIr(){
  canvas.fillSprite(COL_BG); statusBar();
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  // hint sul comando alternativo (KEY2 commuta)
  canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.drawString(irMode ? "KEY2 = spegni" : "KEY2 = accendi", W/2, BAR_H+10);
  // grande pulsante "power": rosso = spegni, verde = accendi
  uint16_t ring = irMode ? TFT_DARKGREEN : TFT_RED;
  canvas.fillCircle(IR_CX,IR_CY,IR_CR,ring); canvas.drawCircle(IR_CX,IR_CY,IR_CR,TFT_WHITE);
  drawPower(IR_CX,IR_CY,22,TFT_WHITE);
  canvas.setTextColor(TFT_WHITE,COL_BG);  canvas.drawString(irMode?"ACCENDI TV":"SPEGNI TV", IR_CX, IR_CY+IR_CR+14);
  canvas.setTextColor(COL_MUTED,COL_BG);  canvas.drawString("tocca = invia", IR_CX, IR_CY+IR_CR+28);
  // marche supportate
  canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.drawString("Samsung  LG  Sony", W/2, H-50);
  canvas.drawString("Panasonic  Hisense", W/2, H-38);
  // feedback invio
  canvas.setTextColor(COL_ACCENT,COL_BG); canvas.drawString(irState, W/2, H-18);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderLora(){
  canvas.fillSprite(COL_BG); statusBar();
  canvas.setTextDatum(middle_center);
  canvas.setTextFont(&fonts::Font4); canvas.setTextColor(TFT_CYAN,COL_BG);
  char fb[10]; sprintf(fb,"%.3f",curFreq); canvas.drawString(fb, W/2, BAR_H+22);
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(TFT_WHITE,COL_BG);
  canvas.drawString("MHz", W/2, BAR_H+42);
  canvas.setTextColor(TFT_GREEN,COL_BG); canvas.drawString(PRESETS[presetIdx].name, W/2, BAR_H+56);
  canvas.setTextDatum(top_left);
  if(!loraOk){ canvas.setTextColor(TFT_RED,COL_BG); canvas.setCursor(6,BAR_H+72); canvas.print("SX1262 KO - antenna?"); }
  else {
    canvas.setTextColor(TFT_WHITE,COL_BG);
    canvas.setCursor(6,94);  canvas.printf("Nodi %d  Pkt %lu  Msg %lu", nodeCount, (unsigned long)pktTot, (unsigned long)pktMsg);
    canvas.setCursor(6,106); canvas.printf("RSSI %ld  SNR %.0f", lastRssi, lastSnr);
    // riquadro con l'ultimo messaggio di testo decodificato
    canvas.drawRoundRect(4,118,W-8,72,4,COL_LINE);
    canvas.setTextColor(COL_ACCENT,COL_BG); canvas.setCursor(8,122); canvas.print("ULTIMO MESSAGGIO");
    if(lastMsgTime){
      canvas.setTextColor(TFT_GREEN,COL_BG); canvas.setCursor(8,134); canvas.printf("da !%08lx",(unsigned long)lastMsgFrom);
      canvas.setTextColor(TFT_WHITE,COL_BG);
      String m=lastMsg; int yy=148;
      for(int k=0;k<3 && m.length()>0;k++){ canvas.setCursor(8,yy); canvas.print(m.substring(0,21)); m = m.length()>21?m.substring(21):""; yy+=12; }
    } else {
      canvas.setTextColor(COL_MUTED,COL_BG); canvas.setCursor(8,148); canvas.print("in ascolto...");
    }
  }
  drawBtn(loraMinus); drawBtn(loraPlus);
  canvas.setTextColor(COL_MUTED,COL_BG); canvas.setTextDatum(middle_center);
  canvas.drawString("KEY2 = cambia preset", W/2, H-42);   // sopra i pulsanti -/+
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderSaver(){
  // --- SETUP 3/4: RISPARMIO ENERGETICO ---
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_ACCENT,COL_BG); canvas.drawString("RISPARMIO", W/2, BAR_H+8);
  canvas.setTextColor(TFT_WHITE,COL_BG);  canvas.drawString("SPEGNI SCHERMO DOPO", W/2, BAR_H+30);
  canvas.setTextFont(&fonts::Font4); canvas.setTextColor(COL_ACCENT,COL_BG);
  canvas.drawString(SCR_TLABEL[scrIdx], W/2, 111);
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.drawString("backlight on/off:", W/2, H-46);
  canvas.drawString("luminosita' non regolabile", W/2, H-34);
  canvas.drawString("KEY2 = wifi", W/2, H-14);
  canvas.setTextDatum(top_left);
  drawBtn(scrMinus); drawBtn(scrPlus);
  canvas.pushSprite(0,0);
}
void renderSetBatt(){
  // --- SETUP 2/4: BATTERIA ---
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_ACCENT,COL_BG); canvas.drawString("BATTERIA", W/2, BAR_H+8);
  canvas.setTextDatum(top_left);
  int mv = M5.Power.getBatteryVoltage();
  int bx=8,by=BAR_H+22,bw=W-54,bh=22;
  canvas.drawRoundRect(bx,by,bw,bh,3,TFT_WHITE); canvas.fillRect(bx+bw,by+6,3,bh-12,TFT_WHITE);
  uint16_t c=gLevel>50?TFT_GREEN:(gLevel>20?TFT_ORANGE:TFT_RED);
  if(gLevel>=0) canvas.fillRect(bx+2,by+2,constrain(gLevel,0,100)*(bw-4)/100,bh-4,c);
  canvas.setTextColor(TFT_WHITE,COL_BG); canvas.setCursor(bx+bw+8,by+7); canvas.printf("%d%%", gLevel);
  canvas.setCursor(8, by+40); canvas.printf("Carica   : %d %%", gLevel);
  canvas.setCursor(8, by+56); canvas.printf("Tensione : %d mV", mv);
  canvas.setCursor(8, by+72); canvas.printf("Ricarica : %s", gChg?"SI (USB)":"no");
  canvas.setCursor(8, by+88); canvas.printf("Stato    : %s", gChg?"in carica":(gLevel<20?"basso":"ok"));
  canvas.setTextColor(COL_MUTED,COL_BG); canvas.setTextDatum(middle_center);
  canvas.drawString("KEY2 = risparmio", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderSetWifi(){
  // --- SETUP 4/4: WIFI (provisioning via BLE) ---
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_ACCENT,COL_BG); canvas.drawString("WIFI (via BLE)", W/2, BAR_H+8);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_WHITE,COL_BG);
  canvas.setCursor(6,BAR_H+26); canvas.print("Rete attuale:");
  canvas.setTextColor(TFT_CYAN,COL_BG);
  canvas.setCursor(6,BAR_H+40); canvas.print(gWifiSsid.length()?gWifiSsid:"(nessuna)");
  canvas.setTextColor(bleConnected?TFT_GREEN:COL_MUTED,COL_BG);
  canvas.setCursor(6,BAR_H+60); canvas.print(bleConnected?"telefono connesso":"in attesa di connessione BLE");
  canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.setCursor(6,BAR_H+80);  canvas.print("Da telefono (nRF Connect /");
  canvas.setCursor(6,BAR_H+92);  canvas.print("LightBlue): connetti a");
  canvas.setCursor(6,BAR_H+104); canvas.print("\"Nesso N1\", scrivi SSID e");
  canvas.setCursor(6,BAR_H+116); canvas.print("poi PASSWORD per salvare.");
  canvas.setTextColor(COL_MUTED,COL_BG); canvas.setTextDatum(middle_center);
  canvas.drawString("tocco = dimentica rete", W/2, H-28);
  canvas.drawString("KEY2 = ora", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderSetup(){
  canvas.fillSprite(COL_BG); statusBar();
  if(setupMode==1){ renderSetBatt(); return; }
  if(setupMode==2){ renderSaver();   return; }
  if(setupMode==3){ renderSetWifi(); return; }
  // --- SETUP 1/4: ORA ---
  canvas.setTextDatum(middle_center);
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(COL_ACCENT,COL_BG);
  canvas.drawString("ORA", W/2, BAR_H+8);
  canvas.setTextFont(&fonts::Font4); canvas.setTextColor(TFT_WHITE,COL_BG);
  canvas.drawString(clockStr().c_str(), W/2, BAR_H+32);
  canvas.setTextDatum(top_left);
  drawBtn(setHm); drawBtn(setHp); drawBtn(setMm); drawBtn(setMp);
  drawBtnC(setNtp, ntpBusy ? TFT_GREEN : setNtp.col);   // si schiarisce al tocco di SYNC
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(TFT_CYAN,COL_BG);
  canvas.setCursor(6,152); canvas.print(ntpState);
  canvas.setTextColor(COL_MUTED,COL_BG); canvas.setTextDatum(middle_center);
  canvas.drawString("KEY2 = batteria", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}

void renderTorch(){
  uint16_t fill = TORCH_SHADE[torchLevel];
  canvas.fillSprite(fill);
  // testo leggibile: scuro sui toni chiari, chiaro sui toni scuri
  uint16_t txt = (torchLevel>=2) ? TFT_BLACK : TFT_WHITE;
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(txt,fill); canvas.drawString("TORCIA", W/2, H/2-12);
  canvas.setTextFont(&fonts::Font4); canvas.drawString(TORCH_LBL[torchLevel], W/2, H/2+14);
  canvas.setTextFont(&fonts::Font0); canvas.drawString("KEY2 = intensita", W/2, H-16);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderCrono(){
  canvas.fillSprite(COL_BG); statusBar();
  uint32_t e = cronoAcc + (cronoRun ? millis()-cronoStart : 0);
  uint32_t m=e/60000, s=(e/1000)%60, t=(e/100)%10;
  char b[16]; sprintf(b,"%02lu:%02lu.%lu",(unsigned long)m,(unsigned long)s,(unsigned long)t);
  canvas.setTextDatum(middle_center);
  canvas.setTextFont(&fonts::Font4); canvas.setTextColor(cronoRun?TFT_GREEN:TFT_WHITE,COL_BG);
  canvas.drawString(b, W/2, BAR_H+60);
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.drawString(cronoRun?"KEY2 = pausa":"KEY2 = avvia", W/2, H-44);
  canvas.drawString("tocca = azzera", W/2, H-28);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void gameSpawnTarget(){ tgtX = 12 + random(W-24); tgtY = BAR_H+12 + random(H-BAR_H-30); }
void gameReset(){ randomSeed(micros()); ballX=W/2; ballY=H/2; ballVX=ballVY=0; gScore=0; gameSpawnTarget(); gInit=true; }
void renderGame(){
  if(!gInit) gameReset();
  // inclinazione -> accelerazione della biglia (BMI270)
  float ax=0,ay=0,az=0; M5.Imu.update(); M5.Imu.getAccelData(&ax,&ay,&az);
  ballVX += ay*1.4f; ballVY += ax*1.4f;
  ballVX *= 0.90f;  ballVY *= 0.90f;        // attrito
  ballX  += ballVX; ballY  += ballVY;
  // rimbalzo sui bordi (area di gioco sotto la barra, sopra il punteggio)
  const int r=5, top=BAR_H+2, bot=H-16, lft=2, rgt=W-2;
  if(ballX<lft+r){ ballX=lft+r; ballVX=-ballVX*0.5f; }
  if(ballX>rgt-r){ ballX=rgt-r; ballVX=-ballVX*0.5f; }
  if(ballY<top+r){ ballY=top+r; ballVY=-ballVY*0.5f; }
  if(ballY>bot-r){ ballY=bot-r; ballVY=-ballVY*0.5f; }
  // bersaglio raggiunto
  float dx=ballX-tgtX, dy=ballY-tgtY;
  if(dx*dx+dy*dy < (r+6)*(r+6)){ gScore++; M5.Speaker.tone(2400,40); gameSpawnTarget(); }
  // disegno
  canvas.fillSprite(COL_BG); statusBar();
  canvas.fillCircle(tgtX,tgtY,6,TFT_YELLOW); canvas.drawCircle(tgtX,tgtY,6,TFT_WHITE);
  canvas.fillCircle((int)ballX,(int)ballY,r,TFT_CYAN); canvas.drawCircle((int)ballX,(int)ballY,r,TFT_WHITE);
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center); canvas.setTextColor(TFT_WHITE,COL_BG);
  char b[32]; sprintf(b,"Punti: %d   KEY2=reset", gScore); canvas.drawString(b, W/2, H-8);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderMouse(){
  canvas.fillSprite(COL_BG); statusBar();
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  bool conn = mouseInited && bleConnected;
  canvas.setTextColor(conn?TFT_GREEN:TFT_ORANGE, COL_BG);
  canvas.drawString(conn?"BLE: connesso":"BLE: in attesa...", W/2, BAR_H+16);
  canvas.setTextColor(COL_MUTED, COL_BG);
  canvas.drawString(conn?"":"accoppia da Windows", W/2, BAR_H+32);

  // pallino centrale (vero joystick): direzione = verso il bordo, distanza =
  // velocita'; torna SEMPRE al centro appena il polso torna alla posa calibrata.
  int cx=W/2, cy=BAR_H+110, R=56;
  canvas.drawCircle(cx,cy,R,TFT_DARKGREY);
  canvas.drawLine(cx-R,cy,cx+R,cy,0x3186);
  canvas.drawLine(cx,cy-R,cx,cy+R,0x3186);
  int px = cx + (int)constrain(sAX/MOUSE_MAXTILT*R, -R, R);
  int py = cy - (int)constrain(sAY/MOUSE_MAXTILT*R, -R, R);
  float dd = sqrtf(sAX*sAX+sAY*sAY);
  uint16_t bc = mouseHold?TFT_RED:(dd<MOUSE_DEAD?TFT_DARKGREY:TFT_CYAN);
  canvas.fillCircle(px,py,9,bc);
  canvas.drawCircle(px,py,9,TFT_WHITE);

  canvas.setTextColor(mouseHold?TFT_RED:COL_MUTED, COL_BG);
  canvas.drawString(mouseHold?"BLOCCATO (tocca per riprendere)":"tocca = blocca puntatore", W/2, H-40);
  canvas.setTextColor(COL_MUTED, COL_BG);
  canvas.drawString("KEY2 = click sinistro", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderGpsStat(){
  bool conn = gpsModuleConnected();
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(conn?TFT_GREEN:TFT_RED, COL_BG);
  canvas.drawString(conn?"MODULO: connesso":"MODULO: nessun dato", W/2, BAR_H+14);
  canvas.setTextDatum(top_left);

  int fx = gpsFixType();
  const char* fixLbl = (fx==3)?"FIX 3D":(fx==2)?"FIX 2D":(conn?"in ricerca...":"---");
  uint16_t fixCol = (fx==3)?TFT_GREEN:(fx==2)?TFT_ORANGE:COL_MUTED;
  canvas.setTextFont(&fonts::Font4); canvas.setTextDatum(middle_center);
  canvas.setTextColor(fixCol, COL_BG);
  canvas.drawString(fixLbl, W/2, BAR_H+50);
  canvas.setTextDatum(top_left);

  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(TFT_WHITE,COL_BG);
  int y = BAR_H+82;
  canvas.setCursor(10,y); canvas.printf("Satelliti : %d", gps.satellites.isValid()?gps.satellites.value():0); y+=16;
  canvas.setCursor(10,y); canvas.printf("HDOP      : %s", gps.hdop.isValid()?String(gps.hdop.hdop(),1).c_str():"---"); y+=16;
  float pd=gpsPdop(), vd=gpsVdop();
  canvas.setCursor(10,y); canvas.printf("PDOP      : %s", pd>=0?String(pd,1).c_str():"---"); y+=16;
  canvas.setCursor(10,y); canvas.printf("VDOP      : %s", vd>=0?String(vd,1).c_str():"---"); y+=16;
  canvas.setCursor(10,y); canvas.printf("Frasi OK  : %lu", (unsigned long)gps.passedChecksum()); y+=16;
  canvas.setCursor(10,y); canvas.printf("Frasi err : %lu", (unsigned long)gps.failedChecksum()); y+=16;
}
void renderGpsPos(){
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(COL_ACCENT, COL_BG);
  canvas.drawString("POSIZIONE", W/2, BAR_H+12);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_WHITE,COL_BG);
  int y = BAR_H+30;
  if (gps.location.isValid()){
    canvas.setCursor(6,y); canvas.printf("Lat  %.6f", gps.location.lat()); y+=15;
    canvas.setCursor(6,y); canvas.printf("Lon  %.6f", gps.location.lng()); y+=15;
  } else { canvas.setCursor(6,y); canvas.print("Lat/Lon : ---"); y+=15; canvas.setCursor(6,y); canvas.print("(nessun fix)"); y+=15; }
  canvas.setCursor(6,y); canvas.printf("Alt  : %s m", gps.altitude.isValid()?String(gps.altitude.meters(),1).c_str():"---"); y+=15;
  canvas.setCursor(6,y); canvas.printf("Vel  : %s km/h", gps.speed.isValid()?String(gps.speed.kmph(),1).c_str():"---"); y+=15;
  canvas.setCursor(6,y);
  if (gps.course.isValid()) canvas.printf("Rotta: %.0f (%s)", gps.course.deg(), TinyGPSPlus::cardinal(gps.course.deg()));
  else canvas.print("Rotta: ---");
  y+=15;
  canvas.setCursor(6,y); canvas.print("Data : ");
  if (gps.date.isValid()) canvas.printf("%02d/%02d/%04d", gps.date.day(), gps.date.month(), gps.date.year());
  else canvas.print("---");
  y+=15;
  canvas.setCursor(6,y); canvas.print("UTC  : ");
  if (gps.time.isValid()) canvas.printf("%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
  else canvas.print("---");
  y+=15;
  canvas.setCursor(6,y);
  if (gps.location.isValid()) canvas.printf("Eta' fix: %lu s", (unsigned long)(gps.location.age()/1000));
  else canvas.print("Eta' fix: ---");
}
void renderGps(){
  canvas.fillSprite(COL_BG); statusBar();
  if (gpsView==0) renderGpsStat(); else renderGpsPos();
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  if (gpsSyncState[0]){
    canvas.setTextColor(COL_ACCENT, COL_BG);
    canvas.drawString(gpsSyncState, W/2, H-28);
  }
  canvas.setTextColor(COL_MUTED, COL_BG);
  canvas.drawString("KEY2=vista  tocco=sync ora", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderWxNow(){
  int cx=W/2, cy=BAR_H+50, r=34;
  drawWxIcon(cx, cy, r, wxCat, millis());
  canvas.setTextFont(&fonts::Font4); canvas.setTextDatum(middle_center); canvas.setTextColor(TFT_WHITE,COL_BG);
  char tb[12]; sprintf(tb,"%.1f C", wxTemp);
  canvas.drawString(tb, W/2, BAR_H+96);
  canvas.setTextFont(&fonts::Font0); canvas.setTextColor(COL_ACCENT,COL_BG);
  canvas.drawString(wxDesc.c_str(), W/2, BAR_H+114);
  canvas.setTextDatum(top_left); canvas.setTextColor(TFT_WHITE,COL_BG);
  int y=BAR_H+130;
  canvas.setCursor(8,y); canvas.printf("Percepita : %.1f C", wxFeels); y+=15;
  canvas.setCursor(8,y); canvas.printf("Umidita'  : %.0f %%", wxHum); y+=15;
  canvas.setCursor(8,y); canvas.printf("Vento     : %.1f km/h", wxWindSpd); y+=15;
  canvas.setTextColor(COL_MUTED,COL_BG);
  canvas.setCursor(8,y); canvas.printf("agg. %lu s fa", (unsigned long)((millis()-wxUpdated)/1000));
}
void renderWxForecast(){
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center); canvas.setTextColor(COL_ACCENT,COL_BG);
  canvas.drawString("PROSSIME ORE", W/2, BAR_H+12);
  canvas.setTextDatum(top_left);
  if (wxFcCount==0){
    canvas.setTextDatum(middle_center); canvas.setTextColor(COL_MUTED,COL_BG);
    canvas.drawString("nessun dato", W/2, BAR_H+70);
    canvas.setTextDatum(top_left);
    return;
  }
  int rowH = (H-BAR_H-40) / wxFcCount;
  for (int i=0;i<wxFcCount;i++){
    int y = BAR_H+24 + i*rowH;
    time_t t=(time_t)wxFcTime[i]; struct tm* ti=localtime(&t);
    char hb[8]; sprintf(hb,"%02d:00", ti?ti->tm_hour:0);
    canvas.fillRoundRect(2,y,W-4,rowH-2,3,COL_PANEL);
    canvas.setTextColor(TFT_WHITE,COL_PANEL); canvas.setTextDatum(middle_left);
    canvas.drawString(hb, 8, y+rowH/2);
    drawWxIcon(W/2, y+rowH/2, 14, wxFcCat[i], millis()+i*400);
    canvas.setTextDatum(middle_right); canvas.setTextColor(TFT_WHITE,COL_PANEL);
    char tb[8]; sprintf(tb,"%.0f C", wxFcTemp[i]);
    canvas.drawString(tb, W-8, y+rowH/2);
    canvas.setTextDatum(top_left);
  }
}
void renderWeather(){
  canvas.fillSprite(COL_BG); statusBar();
  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  bool haveFix = gps.location.isValid();
  canvas.setTextColor(haveFix?TFT_GREEN:TFT_ORANGE, COL_BG);
  canvas.drawString(haveFix?"GPS: fix ok":"GPS: in attesa...", W/2, BAR_H+8);
  canvas.setTextDatum(top_left);

  if (wxHaveData){
    if (wxView==0) renderWxNow(); else renderWxForecast();
  } else {
    canvas.setTextDatum(middle_center); canvas.setTextColor(COL_MUTED,COL_BG);
    canvas.drawString("nessun dato ancora", W/2, BAR_H+90);
    canvas.setTextDatum(top_left);
  }

  canvas.setTextFont(&fonts::Font0); canvas.setTextDatum(middle_center);
  canvas.setTextColor(wxBusy?TFT_ORANGE:COL_MUTED, COL_BG);
  canvas.drawString(wxState.c_str(), W/2, H-28);
  canvas.setTextColor(COL_MUTED, COL_BG);
  canvas.drawString("tocco=vista  KEY2=aggiorna", W/2, H-14);
  canvas.setTextDatum(top_left);
  canvas.pushSprite(0,0);
}
void renderBarOnly(){ statusBar(); canvas.pushSprite(0,0); }
void render(){
  switch(page){
    case PG_COMP:    renderCompanion(); break;
    case PG_DRAW:    renderDraw();    break;
    case PG_LEVEL:   renderLevel();   break;
    case PG_WIFI:    renderWifi();    break;
    case PG_LORA:    renderLora();    break;
    case PG_IR:      renderIr();      break;
    case PG_TORCH:   renderTorch();   break;
    case PG_CRONO:   renderCrono();   break;
    case PG_GAME:    renderGame();    break;
    case PG_MOUSE:   renderMouse();   break;
    case PG_GPS:     renderGps();     break;
    case PG_WEATHER: renderWeather(); break;
    case PG_SETUP:   renderSetup();   break;
  }
}

void doWifiScan(){
  wifiN=-1; renderWifi();
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(50);
  int n=WiFi.scanNetworks(); wifiN=n;
  for(int i=0;i<n && i<10;i++){ wSSID[i]=WiFi.SSID(i); wRSSI[i]=WiFi.RSSI(i); }
  Serial.printf("[WiFi] %d reti\n", n);
  WiFi.mode(WIFI_OFF);     // spengo la radio dopo la scansione (niente RF di fondo)
}

void sleepScreen(){           // backlight OFF (E1.P2 e' on/off, non dimmerabile)
  if(!screenOn) return;
  M5.Display.setBrightness(0); M5.Display.sleep(); setLed(false);
  screenOn=false;
}
void wakeScreen(){            // backlight ON
  if(screenOn) return;
  M5.Display.wakeup(); M5.Display.setBrightness(255);
  screenOn=true; lastActivity=millis(); touchPrev=true;   // il tocco di risveglio non vale come tap
  render();
}
void onEnter(){
  if(page==PG_DRAW){ canvas.fillSprite(TFT_BLACK); lastDX=lastDY=-1; }
  // LoRa: ricevo SOLO sulla pagina LORA; altrove il radio va in standby
  // (niente transitori RF in background che falsavano i pulsanti).
  if(page==PG_LORA){ if(!loraReady) loraInit(); else if(loraOk) radio.startReceive(); }
  else if(loraReady && loraOk) radio.standby();
  if(page==PG_IR)    irInit();
  if(page==PG_SETUP) setupMode=0;
  if(page==PG_GAME)  gameReset();
  if(page==PG_MOUSE){ mouseInit(); mouseCalibrate(); mouseHold=false; }
  if(page==PG_GPS)     { gpsInit(); gpsView=0; gpsSyncState=""; }
  if(page==PG_WEATHER) { gpsInit(); }   // serve il GPS anche qui per la posizione
  M5.Display.setRotation(page==PG_COMP ? 1 : 0);   // COMPANION in orizzontale, il resto in verticale
  M5.Display.setBrightness(255);   // backlight sempre acceso (no PWM su questo HW)
  render();
}
void key2Action(){
  switch(page){
    case PG_DRAW:  canvas.fillRect(0,BAR_H,W,H-BAR_H-PAL_H,TFT_BLACK); break;
    case PG_LEVEL: levelMode ^= 1; break;
    case PG_WIFI:  doWifiScan(); break;
    case PG_LORA:  presetIdx=(presetIdx+1)%NUM_PRESETS; loraResetStats(); applyPreset(presetIdx); break;
    case PG_IR:    irMode ^= 1; break;   // commuta tra SPEGNI e ACCENDI (invio col tocco)
    case PG_TORCH: torchLevel=(torchLevel+1)%TORCH_N; break;
    case PG_CRONO: if(cronoRun){ cronoAcc+=millis()-cronoStart; cronoRun=false; } else { cronoStart=millis(); cronoRun=true; } break;
    case PG_GAME:  gameReset(); break;
    case PG_MOUSE: mouseClick(); break;
    case PG_GPS:     gpsView=(gpsView+1)%GPS_VIEWS; break;
    case PG_WEATHER: weatherFetch(); break;
    case PG_SETUP:
      setupMode=(setupMode+1)%NUM_SETUP;
      if(setupMode==3) wifiCfgInit();   // avvia il servizio BLE solo quando serve davvero
      break;
    default: break;
  }
  render();
}
void handleTap(int x,int y){
  if(page==PG_DRAW){
    if(y>=H-PAL_H){ int sw=W/NPEN; int i=x/sw; if(i>=0&&i<NPEN) penIdx=i; render(); }
    return;
  }
  if(page==PG_WIFI && y<BAR_H){ doWifiScan(); return; }
  if(page==PG_LEVEL){
    if(levelMode==1){
      if(!angTared){ angOffset=lastAng; angTared=true; }
      else { angTared=false; angOffset=0; }
      render();
    }
    return;
  }
  if(page==PG_LORA){
    if(hit(loraMinus,x,y)) nudgeFreq(-0.1f);
    else if(hit(loraPlus,x,y)) nudgeFreq(0.1f);
    render(); return;
  }
  if(page==PG_IR){
    if((x-IR_CX)*(x-IR_CX)+(y-IR_CY)*(y-IR_CY) < IR_CR*IR_CR){
      irState = irMode ? "accensione..." : "spegnimento..."; render();
      if(irMode) irTvOn(); else irTvOff();
      irState="codici inviati"; irMsg=millis();
    }
    render(); return;
  }
  if(page==PG_CRONO){ cronoAcc=0; if(cronoRun) cronoStart=millis(); render(); return; }
  if(page==PG_MOUSE){
    mouseHold = !mouseHold;
    if(!mouseHold) mouseCalibrate();   // ricalibra il centro quando si riprende
    render(); return;
  }
  if(page==PG_GPS){ gpsTimeSync(); render(); return; }
  if(page==PG_WEATHER){ wxView=(wxView+1)%2; render(); return; }
  if(page==PG_SETUP){
    if(setupMode==0){               // ORA
      if(hit(setHm,x,y)) shiftTime(-3600);
      else if(hit(setHp,x,y)) shiftTime(3600);
      else if(hit(setMm,x,y)) shiftTime(-60);
      else if(hit(setMp,x,y)) shiftTime(60);
      else if(hit(setNtp,x,y)) ntpSyncUI();
    } else if(setupMode==2){        // RISPARMIO
      if(hit(scrMinus,x,y)) scrIdx=(scrIdx+NUM_SCR-1)%NUM_SCR;
      else if(hit(scrPlus,x,y)) scrIdx=(scrIdx+1)%NUM_SCR;
    } else if(setupMode==3){        // WIFI: tocco = dimentica la rete salvata
      wifiCredsForget();
    }                               // setupMode 1 (BATTERIA) = sola lettura
    render(); return;
  }
}

// ================= SETUP / LOOP ===============================
void setup(){
  auto cfg = M5.config(); cfg.serial_baudrate=115200; M5.begin(cfg);
  M5.Display.setRotation(0);              // VERTICALE 135x240
  M5.Speaker.setVolume(150);
  canvas.createSprite(W,H); canvas.setTextFont(&fonts::Font0);
  setenv("TZ", TZ_INFO, 1); tzset();
  shiftTime(0);                            // attiva clock; default poi impostabile
  { struct timeval tv; tv.tv_sec=1717243200; tv.tv_usec=0; settimeofday(&tv,nullptr); } // ~01/06/2024 12:00 UTC
  wifiCredsLoad();                         // se e' stata salvata una rete via BLE, usa quella
  // BLE sempre attivo dal boot (prima partiva solo entrando in MOUSE o in
  // SETUP>WIFI, quindi il dispositivo non appariva finche' non ci navigavi):
  // cosi' e' visibile come "Nesso N1" subito all'accensione, sia per il mouse
  // HID sia per scrivere le credenziali WiFi.
  mouseInit();
  wifiCfgInit();
  bleAdvertiseStart();
  Serial.println("\n=== Nesso N1 Suite (verticale) ===");
  M5.Speaker.tone(2000,90);
  lastActivity = millis();
  onEnter();
}

void loop(){
  M5.update();
  companionPollSerial();   // dati dal PC (Desktop Companion): letti sempre, su qualsiasi pagina
  // batteria: leggo il fuel gauge solo ogni 2 s. L'I2C continuo ad ogni loop
  // disturbava la lettura dei pulsanti (anch'essi su I2C) -> falsi KEY1.
  static uint32_t lastBatt=0;
  if(gLevel<0 || millis()-lastBatt>2000){
    lastBatt=millis(); gLevel=M5.Power.getBatteryLevel(); gChg=((int)M5.Power.isCharging())>0;
  }

  // --- input + risparmio schermo ---
  // KEY1/KEY2 con debounce a 60 ms per ignorare i falsi tocchi da glitch I2C
  static Btn k1, k2;
  bool btnA = keyPressed(k1, M5.BtnA.isPressed(), 60);
  bool btnB = keyPressed(k2, M5.BtnB.isPressed(), 60);
  bool down = M5.Touch.getCount()>0; auto t=M5.Touch.getDetail();
  int tx=t.x, ty=t.y;
  if(btnA || btnB || down) lastActivity=millis();

  if(!screenOn){                          // schermo spento: il primo input riaccende e basta
    if(btnA || btnB || down) wakeScreen();
    if(page==PG_LORA) handleLoraRx();     // ricevo solo se sono sulla pagina LoRa
    if(page==PG_GPS)  handleGpsRx();      // ricevo solo se sono sulla pagina GPS
    return;
  }
  uint32_t scrTo = SCR_TIMEOUTS[scrIdx];
  bool compLive = page==PG_COMP && companionPcConnected();   // il companion resta acceso col PC collegato
  if(scrTo>0 && page!=PG_TORCH && page!=PG_GAME && !compLive && millis()-lastActivity>scrTo){ sleepScreen(); return; }

  if(millis()-lastBlink>800){ lastBlink=millis(); ledOn=!ledOn; setLed(ledOn); }

  if(btnA){ page=(page+1)%PG_COUNT; onEnter(); }   // KEY1 scorre le pagine
  if(page==PG_COMP) companionKey(k2.state);        // COMPANION: KEY2 breve/lungo come il tasto del robot
  else if(btnB){ key2Action(); }                   // KEY2 azione della pagina

  if(down && !touchPrev) handleTap(tx,ty);
  // disegno continuo
  if(page==PG_DRAW && down && ty>=BAR_H && ty<H-PAL_H){
    if(lastDX<0){ lastDX=tx; lastDY=ty; }
    drawSeg(lastDX,lastDY,tx,ty,penCols[penIdx],penR);
    lastDX=tx; lastDY=ty; canvas.pushSprite(0,0);
  }
  if(!down) lastDX=lastDY=-1;
  touchPrev=down;

  if(page==PG_LORA) handleLoraRx();        // ricevo/decodifico solo sulla pagina LoRa
  if(page==PG_GPS || page==PG_WEATHER) handleGpsRx();   // leggo/parso l'NMEA (serve anche al METEO per le coordinate)

  if(page==PG_IR && irMsg && millis()-irMsg>1500){ irMsg=0; irState="pronto"; }

  // air-mouse: aggiorno il movimento ad ogni giro di loop (non solo al refresh
  // schermo) cosi' il puntatore resta fluido anche col display a 120 ms.
  if(page==PG_MOUSE) mouseUpdate();

  // refresh: il gioco e il mouse girano piu' veloci (~25 fps), le altre pagine ogni 120 ms
  static uint32_t lr=0, lg=0;
  if(page==PG_COMP || page==PG_GAME || page==PG_MOUSE || (page==PG_WEATHER && wxHaveData && wxView==0)){ if(millis()-lg>40){ lg=millis(); render(); } }
  else if(millis()-lr>120){ lr=millis(); render(); }
}