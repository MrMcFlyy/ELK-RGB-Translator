#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// ==== App-Seite (Duoco/Magic Lantern, ELK-BLEDOM) ====
static BLEUUID APP_SERVICE_UUID((uint16_t)0xFFF0);
static BLEUUID APP_CHAR_UUID   ((uint16_t)0xFFF3);

// ==== Lampen-Seite (LED Lamp = FFE0/FFE1) ====
static BLEUUID LAMP_SERVICE_UUID((uint16_t)0xFFE0);
static BLEUUID LAMP_CHAR_UUID   ((uint16_t)0xFFE1);

// Deine Lampe (MAC & Name):
static const char* LAMP_ADDR_STR = "XX:XX:XX:XX:YZ:AB";
static const char* LAMP_NAME     = "LEDBLE-00-YZAB";

// --- Server (App) ---
BLEServer*         appServer = nullptr;
BLECharacteristic* appChar   = nullptr;
volatile bool      appConnected = false;

// --- Client (Lampe) ---
BLEClient*               lampClient = nullptr;
BLERemoteCharacteristic* lampChar   = nullptr;
bool                     lampConnected = false;
unsigned long            lastLampTry = 0;

// --- Handshake / Heartbeat (App-Seite) ---
unsigned long tConnected=0, lastHelloTry=0, lastHeartbeat=0;
int helloTries=0;

// ------------------------------------------------------------
// Helper
// ------------------------------------------------------------
static String toHex(const uint8_t* d, size_t n){
  static const char* H="0123456789ABCDEF"; String o; o.reserve(n*3);
  for(size_t i=0;i<n;i++){ uint8_t b=d[i]; o+=H[b>>4]; o+=H[b&0xF]; o+=' '; }
  return o;
}

// ------------------------------------------------------------
// Frame-Erkennung
// ------------------------------------------------------------

// LED Lamp / ML Brightness (bereits Ziel-Format):
// 7E FF 01 LVL 00 FF FF FF EF   (LVL 0..0x64)
static inline bool isLEDBrightnessFrame(const uint8_t* p, size_t n) {
  return n==9 && p[0]==0x7E && p[1]==0xFF && p[2]==0x01 && p[8]==0xEF;
}

// Duoco Brightness (Quelle, die wir umwandeln wollen):
// 7E 04 01 LVL 01 FF FF 00 EF   (LVL 0..0x64)
static inline bool isDuocoBrightnessFrame(const uint8_t* p, size_t n) {
  return n==9 && p[0]==0x7E && p[1]==0x04 && p[2]==0x01 && p[4]==0x01 && p[8]==0xEF;
}

// Power (variante 1 – LED Lamp):
// 7E FF 04 01/00 FF FF FF FF EF
static inline bool isMLPowerFFFrame(const uint8_t* p, size_t n, uint8_t& state) {
  if (n==9 && p[0]==0x7E && p[1]==0xFF && p[2]==0x04 && p[8]==0xEF) { state = p[3]; return true; }
  return false;
}

// Power (variante 2 – manche Duoco):
// 7E 04 04 01/00 ?? ?? ?? ?? EF
static inline bool isMLPower0404Frame(const uint8_t* p, size_t n, uint8_t& state) {
  if (n>=5 && p[0]==0x7E && p[1]==0x04 && p[2]==0x04 && p[n-1]==0xEF) { state = p[3]; return true; }
  return false;
}

// ------------------------------------------------------------
// Builder
// ------------------------------------------------------------

// Ziel: LED Lamp Brightness (FF 01 ...)
static void buildLEDBrightness(uint8_t lvl01_100, uint8_t out[9]) {
  if (lvl01_100 > 0x64) lvl01_100 = 0x64;
  out[0]=0x7E; out[1]=0xFF; out[2]=0x01; out[3]=lvl01_100; out[4]=0x00;
  out[5]=0xFF; out[6]=0xFF; out[7]=0xFF; out[8]=0xEF;
}

// Ziel: LED Lamp Power (FF 04 ...)
static void buildLEDPower(uint8_t onOff, uint8_t out[9]) {
  out[0]=0x7E; out[1]=0xFF; out[2]=0x04; out[3]=onOff ? 0x01 : 0x00;
  out[4]=0xFF; out[5]=0xFF; out[6]=0xFF; out[7]=0xFF; out[8]=0xEF;
}

// ------------------------------------------------------------
// Lampen-Scan (Fallback per Name) & Connect
// ------------------------------------------------------------
bool findLampByName(BLEAddress& out) {
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(40);
  Serial.printf("[LAMP] Scanne nach \"%s\" ...\n", LAMP_NAME);

  BLEScanResults* res = scan->start(5, false);
  if (!res) { Serial.println("[LAMP] Keine Scan-Ergebnisse."); return false; }

  for (int i = 0; i < res->getCount(); ++i) {
    BLEAdvertisedDevice dev = res->getDevice(i);
    String name = String(dev.getName().c_str());
    if (name.length() > 0 && name == LAMP_NAME) {
      out = dev.getAddress();
      Serial.printf("[LAMP] Gefunden: %s @ %s\n", name.c_str(), out.toString().c_str());
      scan->clearResults();
      return true;
    }
  }
  scan->clearResults();
  Serial.println("[LAMP] Nicht gefunden.");
  return false;
}

bool connectLamp() {
  if (lampConnected && lampClient && lampClient->isConnected() && lampChar) return true;

  unsigned long now = millis();
  if (now - lastLampTry < 2000) return false;
  lastLampTry = now;

  if (!lampClient) lampClient = BLEDevice::createClient();

  BLEAddress addr(LAMP_ADDR_STR);
  Serial.printf("[LAMP] Verbinde zu %s ...\n", addr.toString().c_str());
  if (!lampClient->connect(addr)) {
    BLEAddress found("");
    if (findLampByName(found)) {
      Serial.printf("[LAMP] Verbinde (per Scan) zu %s ...\n", found.toString().c_str());
      if (!lampClient->connect(found)) {
        Serial.println("[LAMP] Connect fehlgeschlagen (Scan-Addr).");
        lampConnected = false; lampChar=nullptr;
        return false;
      }
    } else {
      Serial.println("[LAMP] Connect fehlgeschlagen.");
      lampConnected = false; lampChar=nullptr;
      return false;
    }
  }

  BLERemoteService* svc = lampClient->getService(LAMP_SERVICE_UUID);
  if (!svc) {
    Serial.println("[LAMP] Service FFE0 nicht gefunden");
    lampClient->disconnect();
    lampConnected = false; lampChar=nullptr;
    return false;
  }
  lampChar = svc->getCharacteristic(LAMP_CHAR_UUID);
  if (!lampChar) {
    Serial.println("[LAMP] Char FFE1 nicht gefunden");
    lampClient->disconnect();
    lampConnected = false; lampChar=nullptr;
    return false;
  }

  lampConnected = true;
  Serial.println("[LAMP] Verbunden & bereit (FFE0/FFE1).");
  return true;
}

// ------------------------------------------------------------
// Weiterleitung zur Lampe
// ------------------------------------------------------------
void forwardToLamp(const uint8_t* data, size_t len) {
  if (!connectLamp()) {
    Serial.println("[LAMP] (noch) nicht verbunden – Paket verworfen");
    return;
  }
  bool ok = lampChar->writeValue((uint8_t*)data, len, false); // WriteWithoutResponse
  Serial.printf("[LAMP] -> (%u B) %s  %s\n", (unsigned)len, toHex(data,len).c_str(), ok?"[OK]":"[FAIL]");
}

// ------------------------------------------------------------
// App-Server (Empfang von der App)
// ------------------------------------------------------------
class AppServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    appConnected = true;
    tConnected=millis(); lastHelloTry=0; helloTries=0; lastHeartbeat=tConnected;
    Serial.println("[APP ] Connected");
  }
  void onDisconnect(BLEServer*) override {
    appConnected = false;
    Serial.println("[APP ] Disconnected -> advertising");
    delay(100);
    BLEDevice::startAdvertising();
  }
};

class AppRxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    size_t len = c->getLength();
    const uint8_t* data = c->getData();
    if (!data || !len) return;

    Serial.printf("[APP ] RX (%u B): %s\n", (unsigned)len, toHex(data,len).c_str());

    // === Brightness ===
    if (isDuocoBrightnessFrame(data, len)) {
      // Duoco -> LED Lamp
      uint8_t lvl = data[3]; if (lvl > 0x64) lvl = 0x64;
      uint8_t pkt[9]; buildLEDBrightness(lvl, pkt);
      Serial.printf("[MAP ] Duoco Brightness %u%% → LED Lamp\n", lvl);
      forwardToLamp(pkt, sizeof(pkt));
      return;
    }
    if (isLEDBrightnessFrame(data, len)) {
      // bereits LED Lamp-Format -> durchlassen
      uint8_t lvl = data[3]; if (lvl > 0x64) lvl = 0x64;
      Serial.printf("[MAP ] LED Brightness %u%% (pass-through)\n", lvl);
      forwardToLamp(data, len);
      return;
    }

    // === Power ===  (beide Varianten akzeptieren, immer in LED-Format umwandeln)
    uint8_t pwr = 0xFF;
    if (isMLPowerFFFrame(data, len, pwr) || isMLPower0404Frame(data, len, pwr)) {
      uint8_t pkt[9]; buildLEDPower(pwr ? 1 : 0, pkt);
      Serial.printf("[MAP ] Power %s → LED Lamp\n", pwr ? "ON" : "OFF");
      forwardToLamp(pkt, sizeof(pkt));
      return;
    }

    // === Sonst: 1:1 (z. B. Farbe 7E 07 05 03 RR GG BB 10 EF) ===
    forwardToLamp(data, len);

    // optionales Echo zurück an die App (wird ignoriert, wenn nicht subscribed)
    c->setValue((uint8_t*)data, len);
    c->notify();

    lastHeartbeat = millis();
  }
};

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ELK-BLEDOM → LED LAMP Bridge (Duoco→LED Brightness, Power mapped) ===");

  BLEDevice::init("ELK-BLEDOM");
  BLEDevice::setMTU(23); // konservativ & stabil

  // --- App-Server ---
  appServer = BLEDevice::createServer();
  appServer->setCallbacks(new AppServerCb());

  BLEService* appSvc = appServer->createService(APP_SERVICE_UUID);
  appChar = appSvc->createCharacteristic(
    APP_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR |
    BLECharacteristic::PROPERTY_WRITE    |
    BLECharacteristic::PROPERTY_NOTIFY   |
    BLECharacteristic::PROPERTY_READ
  );
  appChar->setCallbacks(new AppRxCb());

  uint8_t initVal[] = {0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xEF};
  appChar->setValue(initVal, sizeof(initVal));
  appSvc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(APP_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("[APP ] Advertising als ELK-BLEDOM (FFF0/FFF3) ...");

  // Erste Lampen-Verbindung anstoßen
  connectLamp();
}

// ------------------------------------------------------------
// Loop
// ------------------------------------------------------------
void loop() {
  const unsigned long now = millis();

  // Fallback-HELLO an App (einige Builds erwarten früh ein Notify)
  if (appConnected) {
    if ((now - tConnected > 150) && (helloTries < 10) && (now - lastHelloTry > 300)) {
      uint8_t hello[] = {0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xEF};
      appChar->setValue(hello, sizeof(hello));
      appChar->notify(); // ignoriert, falls App nicht subscribed
      lastHelloTry = now; helloTries++;
    }
  }

  // Heartbeat (freundlich)
  if (appConnected && (now - lastHeartbeat > 1500)) {
    uint8_t hb[] = {0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xEF};
    appChar->setValue(hb, sizeof(hb));
    appChar->notify();
    lastHeartbeat = now;
  }

  // Reconnect zur Lampe bei Bedarf
  if (!lampConnected || !lampClient || !lampClient->isConnected()) {
    lampConnected = false;
    connectLamp();
  }
}
