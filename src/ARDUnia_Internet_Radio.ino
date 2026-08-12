/*
  ==========================================================================
   Internet Radio with ESP8266 (NodeMCU) + I2S MAX98357A + ST7735 Display
   - Menu-driven station selection with rotary encoder
   - Graphical WiFi signal indicator
   - Web interface for managing station list (stored in SPIFFS)
   - WiFi setup portal (AP mode)
   - All texts in English
   - Optimized for speed, reduced flicker, and memory stability
   - Auto-pause playback during web requests
   - by : ARDUnia [Hamidreza Milaninia]
  ==========================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>
#include <WiFiUdp.h>
#include <PersianDate.h>
#include <FS.h>                 // SPIFFS
#include <ArduinoJson.h>        // version 6.x

#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

// -------------------------- Memory / Buffer Settings --------------------------
#define JSON_BUFFER_SIZE  2048
#define AUDIO_BUFFER_SIZE 1024

// -------------------------- WiFi configuration --------------------------
#define EEPROM_SIZE 128
#define WIFI_MAGIC  0x52414431UL
#define AP_SSID     "InternetRadio-Setup"

struct WiFiCredentials {
  uint32_t magic;
  char ssid[33];
  char password[65];
};

WiFiCredentials wifiCreds;
ESP8266WebServer webServer(80);
bool setupPortalActive = false;
bool webServerStarted = false;
uint8_t wifiFailAttempts = 0;

// -------------------------- I2S pins (fixed) --------------------------
// BCLK = GPIO15 (D8) , LRC = GPIO2 (D4) , DIN = GPIO3 (RX)

// -------------------------- ST7735 Display --------------------------
#define TFT_CS    5    // D1 -> GPIO5
#define TFT_DC    4    // D2 -> GPIO4
#define TFT_RST   -1   // connected to board RST
// SCK = GPIO14 (D5) , MOSI = GPIO13 (D7) , MISO = GPIO12 (D6)

// -------------------------- Rotary Encoder --------------------------
#define ENC_CLK 12   // D6 / GPIO12
#define ENC_DT  16   // D0 / GPIO16 (external pull-up required)
#define ENC_SW   0   // D3 / GPIO0

// -------------------------- Volume Potentiometer --------------------------
#define VOL_PIN   A0

// -------------------------- Objects --------------------------
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
PersianDate pd;

AudioFileSourceICYStream *audioFile   = nullptr;
AudioFileSourceBuffer    *audioBuff   = nullptr;
AudioGeneratorMP3        *audioMP3    = nullptr;
AudioOutputI2S           *audioOut    = nullptr;

// -------------------------- Station Management --------------------------
#define MAX_STATIONS 50
struct RadioStation {
  String name;
  String url;
};

RadioStation stations[MAX_STATIONS];
int stationCount = 0;

// Default station list (fallback)
const char* defaultStations[][2] = {
  {"Radio Swiss Pop",     "http://stream.srg-ssr.ch/m/rsp/mp3_128"},
  {"Classic FM",          "http://media-ice.musicradio.com/ClassicFMMP3"},
  {"France Info",         "http://direct.franceinfo.fr/live/franceinfo-lofi.mp3"},
  {"France Inter",        "http://direct.franceinter.fr/live/franceinter-midfi.mp3"},
  {"FIP Radio",           "http://icecast.radiofrance.fr/fip-midfi.mp3"},
  {"RTL",                 "http://icecast.rtl.fr/rtl-1-44-64"},
  {"RTL2",                "http://icecast.rtl2.fr/rtl2-1-44-128"},
  {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
  {"SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3"},
  {"SomaFM Indie Pop",    "http://ice3.somafm.com/indiepop-128-mp3"},
  {"SomaFM Fluid",        "http://ice3.somafm.com/fluid-128-mp3"},
  {"SomaFM The Trip",     "http://ice3.somafm.com/thetrip-128-mp3"},
  {"SomaFM Vaporwaves",   "http://ice6.somafm.com/vaporwaves-128-mp3"},
  {"SomaFM Lush",         "http://ice3.somafm.com/lush-128-mp3"},
};
const int defaultCount = sizeof(defaultStations) / sizeof(defaultStations[0]);

// Forward declarations
void connectToStation(int idx);
void stopAudio();
void pausePlayback();
void resumePlayback();
void updateStationUI();
void updateVolumeUI();
void updateClockUI();
void startSetupPortal();
void handleSetupPortal();
void connectWiFi();
void handleEncoderStep();
void handleButton();
void handleVolume();
void updateDateTime();
void onEncoderStep(int dir);
void openMenu();
void closeMenu(bool selectStation);
void moveMenuHighlight(int dir);
void adjustMenuScroll();
void drawMenu();
void drawWifiSignal();
bool loadStationsFromSPIFFS();
void saveStationsToSPIFFS();
void resetToDefaultStations();
void setupWebServer();

// -------------------------- State Variables --------------------------
int    currentStation    = 0;
int    volume             = 60;
bool   muted              = false;
String nowPlaying         = "";
bool   nowPlayingChanged  = true;
String persianDateStr     = "";
int    hh = 0, mm = 0, ss = 0;
bool   connectionError    = false;

// -------------------------- Web Request Management --------------------------
bool webRequestActive = false;
unsigned long webRequestStartTime = 0;
bool wasPlayingBeforeWeb = false;

// -------------------------- Rotary Encoder (Polling) --------------------------
uint8_t encLastState = 0;
int8_t encAccumulator = 0;

// -------------------------- Menu --------------------------
bool   menuActive        = false;
int    menuHighlight     = 0;
int    menuScrollOffset  = 0;
unsigned long menuLastActivity = 0;
const unsigned long MENU_TIMEOUT   = 6000; // ms
const int  MENU_VISIBLE_ROWS = 8;
const int  MENU_ROW_H        = 13;
const int  MENU_TOP_Y         = 18;

// -------------------------- NTP --------------------------
WiFiUDP ntpUDP;
const uint16_t NTP_LOCAL_PORT = 2390;
const unsigned long NTP_INTERVAL = 6UL * 60UL * 60UL * 1000UL;
const long IRAN_OFFSET = 12600;
const char* ntpServers[] = {
  "time.google.com",
  "time.cloudflare.com",
  "pool.ntp.org"
};
const uint8_t NTP_SERVER_COUNT = sizeof(ntpServers) / sizeof(ntpServers[0]);
uint32_t syncedEpochLocal = 0;
unsigned long syncedMillis = 0;
bool timeSynced = false;
unsigned long lastNtpSync = 0;

// -------------------------- WiFi signal optimization --------------------------
int lastRSSI = 0;
unsigned long lastWifiUpdate = 0;

// ==========================================================================
//  HTML page for station management (stored in PROGMEM to save RAM)
// ==========================================================================
const char managePageHTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Radio Station Manager</title>
  <style>
    body { font-family: Arial; margin: 20px; max-width: 600px; }
    table { width: 100%; border-collapse: collapse; }
    th, td { border: 1px solid #ddd; padding: 6px; text-align: left; }
    th { background: #167d78; color: white; }
    .actions { display: flex; gap: 8px; }
    .actions button { padding: 4px 8px; }
    .add-form { margin-top: 20px; padding: 12px; background: #f0f0f0; }
    .add-form input { width: 100%; margin: 6px 0; padding: 6px; box-sizing: border-box; }
    .add-form button { padding: 8px 16px; }
    .error { color: red; }
  </style>
</head>
<body>
  <h2>Radio Station List</h2>
  <div id="stations"></div>
  <div class="add-form">
    <h3>Add New Station</h3>
    <input id="newName" placeholder="Station name">
    <input id="newUrl" placeholder="Stream URL">
    <button onclick="addStation()">Add</button>
    <button onclick="resetStations()">Reset to Default</button>
    <span id="msg" style="margin-left: 10px;"></span>
  </div>
  <script>
    async function fetchStations() {
      const res = await fetch('/stations');
      const data = await res.json();
      let html = '<table><tr><th>#</th><th>Name</th><th>URL</th><th>Actions</th></tr>';
      data.forEach((s, i) => {
        html += `<tr><td>${i+1}</td><td><input id="name${i}" value="${s.name}" style="width:100%"></td>
                <td><input id="url${i}" value="${s.url}" style="width:100%"></td>
                <td><button onclick="updateStation(${i})">Update</button>
                <button onclick="deleteStation(${i})">Delete</button></td></tr>`;
      });
      html += '</table>';
      document.getElementById('stations').innerHTML = html;
    }

    async function deleteStation(idx) {
      if (!confirm('Delete station?')) return;
      const res = await fetch('/stations/delete', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'index=' + idx
      });
      if (res.ok) fetchStations();
      else alert('Delete failed');
    }

    async function updateStation(idx) {
      const name = document.getElementById('name'+idx).value;
      const url = document.getElementById('url'+idx).value;
      const res = await fetch('/stations/update', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `index=${idx}&name=${encodeURIComponent(name)}&url=${encodeURIComponent(url)}`
      });
      if (res.ok) fetchStations();
      else alert('Update failed');
    }

    async function addStation() {
      const name = document.getElementById('newName').value;
      const url = document.getElementById('newUrl').value;
      if (!name || !url) { alert('Fill both fields'); return; }
      const res = await fetch('/stations/add', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: `name=${encodeURIComponent(name)}&url=${encodeURIComponent(url)}`
      });
      if (res.ok) {
        document.getElementById('newName').value = '';
        document.getElementById('newUrl').value = '';
        document.getElementById('msg').innerText = 'Added!';
        fetchStations();
      } else {
        alert('Add failed');
      }
    }

    async function resetStations() {
      if (!confirm('Reset to default stations?')) return;
      const res = await fetch('/stations/reset', { method: 'POST' });
      if (res.ok) fetchStations();
      else alert('Reset failed');
    }

    fetchStations();
  </script>
</body>
</html>
)rawliteral";

// ==========================================================================
//  Station list management (SPIFFS)
// ==========================================================================
void resetToDefaultStations() {
  stationCount = defaultCount;
  for (int i = 0; i < defaultCount; i++) {
    stations[i].name = defaultStations[i][0];
    stations[i].url  = defaultStations[i][1];
  }
  saveStationsToSPIFFS();
}

bool loadStationsFromSPIFFS() {
  if (!SPIFFS.begin()) {
    return false;
  }

  if (!SPIFFS.exists("/stations.json")) {
    resetToDefaultStations();
    SPIFFS.end();
    return true;
  }

  File file = SPIFFS.open("/stations.json", "r");
  if (!file) {
    SPIFFS.end();
    return false;
  }

  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  SPIFFS.end();

  if (error) {
    resetToDefaultStations();
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  stationCount = arr.size();
  if (stationCount > MAX_STATIONS) stationCount = MAX_STATIONS;

  for (int i = 0; i < stationCount; i++) {
    stations[i].name = arr[i]["name"].as<String>();
    stations[i].url  = arr[i]["url"].as<String>();
  }
  return true;
}

void saveStationsToSPIFFS() {
  if (!SPIFFS.begin()) {
    return;
  }

  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < stationCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = stations[i].name;
    obj["url"]  = stations[i].url;
  }

  File file = SPIFFS.open("/stations.json", "w");
  if (!file) {
    SPIFFS.end();
    return;
  }
  serializeJson(doc, file);
  file.close();
  SPIFFS.end();
}

void addStation(const String& name, const String& url) {
  if (stationCount >= MAX_STATIONS) return;
  stations[stationCount].name = name;
  stations[stationCount].url  = url;
  stationCount++;
  saveStationsToSPIFFS();
}

void deleteStation(int index) {
  if (index < 0 || index >= stationCount) return;
  for (int i = index; i < stationCount - 1; i++) {
    stations[i] = stations[i + 1];
  }
  stationCount--;
  saveStationsToSPIFFS();
}

void updateStation(int index, const String& name, const String& url) {
  if (index < 0 || index >= stationCount) return;
  stations[index].name = name;
  stations[index].url  = url;
  saveStationsToSPIFFS();
}

// ==========================================================================
//  Web request helpers (pause/resume playback)
// ==========================================================================
void pausePlayback() {
  if (audioMP3 && audioMP3->isRunning()) {
    audioMP3->stop();
    wasPlayingBeforeWeb = true;
    nowPlaying = "⏸ Paused (Web)";
    if (!menuActive) updateStationUI();
  } else {
    wasPlayingBeforeWeb = false;
  }
}

void resumePlayback() {
  if (wasPlayingBeforeWeb && audioOut) {
    connectToStation(currentStation);
    wasPlayingBeforeWeb = false;
  }
}

void startWebRequest() {
  webRequestActive = true;
  webRequestStartTime = millis();
  pausePlayback();
}

void endWebRequest() {
  webRequestActive = false;
  resumePlayback();
}

// ==========================================================================
//  Web server handlers
// ==========================================================================
void handleStationList() {
  startWebRequest();
  int maxSend = (stationCount > 30) ? 30 : stationCount;
  DynamicJsonDocument doc(JSON_BUFFER_SIZE);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < maxSend; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = stations[i].name;
    obj["url"]  = stations[i].url;
  }
  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
  endWebRequest();
}

void handleAddStation() {
  startWebRequest();
  if (!webServer.hasArg("name") || !webServer.hasArg("url")) {
    webServer.send(400, "text/plain", "Missing name or url");
    endWebRequest();
    return;
  }
  String name = webServer.arg("name");
  String url  = webServer.arg("url");
  if (name.length() == 0 || url.length() == 0) {
    webServer.send(400, "text/plain", "Empty name or url");
    endWebRequest();
    return;
  }
  addStation(name, url);
  webServer.send(200, "text/plain", "OK");
  endWebRequest();
}

void handleDeleteStation() {
  startWebRequest();
  if (!webServer.hasArg("index")) {
    webServer.send(400, "text/plain", "Missing index");
    endWebRequest();
    return;
  }
  int idx = webServer.arg("index").toInt();
  if (idx < 0 || idx >= stationCount) {
    webServer.send(400, "text/plain", "Invalid index");
    endWebRequest();
    return;
  }
  deleteStation(idx);
  webServer.send(200, "text/plain", "OK");
  endWebRequest();
}

void handleUpdateStation() {
  startWebRequest();
  if (!webServer.hasArg("index") || !webServer.hasArg("name") || !webServer.hasArg("url")) {
    webServer.send(400, "text/plain", "Missing args");
    endWebRequest();
    return;
  }
  int idx = webServer.arg("index").toInt();
  if (idx < 0 || idx >= stationCount) {
    webServer.send(400, "text/plain", "Invalid index");
    endWebRequest();
    return;
  }
  String name = webServer.arg("name");
  String url  = webServer.arg("url");
  if (name.length() == 0 || url.length() == 0) {
    webServer.send(400, "text/plain", "Empty name or url");
    endWebRequest();
    return;
  }
  updateStation(idx, name, url);
  webServer.send(200, "text/plain", "OK");
  endWebRequest();
}

void handleResetStations() {
  startWebRequest();
  resetToDefaultStations();
  webServer.send(200, "text/plain", "OK");
  endWebRequest();
}

void handleManagePage() {
  startWebRequest();
  webServer.send_P(200, "text/html", managePageHTML);
  endWebRequest();
}

// ==========================================================================
//  WiFi setup portal handlers
// ==========================================================================
void showSetupScreen(const String& line1, const String& line2 = "") {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 3);
  tft.print("WiFi SETUP");

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, 22);
  tft.print("Connect to:");
  tft.setCursor(2, 34);
  tft.print(AP_SSID);

  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(2, 52);
  tft.print("Open:");
  tft.setCursor(2, 64);
  tft.print("192.168.4.1");

  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(2, 84);
  tft.print(line1);
  if (line2.length()) {
    tft.setCursor(2, 96);
    tft.print(line2);
  }

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, 114);
  tft.print("Select WiFi in browser");
}

void startSetupPortal() {
  setupPortalActive = true;
  WiFi.disconnect();
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress apIP = WiFi.softAPIP();

  webServer.on("/", HTTP_GET, webHandleRoot);
  webServer.on("/connect", HTTP_POST, webHandleConnect);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  webServerStarted = true;

  showSetupScreen("AP: InternetRadio-Setup",
                  String("IP: ") + apIP.toString());
}

void handleSetupPortal() {
  if (!setupPortalActive) return;
  webServer.handleClient();
}

void webHandleRoot() {
  String page;
  page.reserve(4000);
  page += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Internet Radio Setup</title>");
  page += F("<style>body{font-family:Arial;margin:20px;max-width:600px}select,input{width:100%;padding:12px;margin:6px 0 14px;box-sizing:border-box}button{padding:12px 20px;font-size:16px}h2{color:#167d78}</style></head><body>");
  page += F("<h2>Internet Radio - WiFi Setup</h2>");
  page += F("<p>Select your WiFi network, enter its password and press Connect.</p>");
  page += F("<form method='POST' action='/connect'>");
  page += F("<label>WiFi Network</label><select name='ssid' required>");
  page += makeScanOptions();
  page += F("</select>");
  page += F("<label>Password</label><input type='password' name='password' maxlength='64' placeholder='WiFi password'>");
  page += F("<button type='submit'>Connect & Save</button></form>");
  page += F("<p>After saving, the radio will restart and connect automatically.</p>");
  page += F("<p><a href='/'>Refresh network list</a></p>");
  page += F("</body></html>");
  webServer.send(200, "text/html; charset=utf-8", page);
}

void webHandleConnect() {
  if (!webServer.hasArg("ssid")) {
    webServer.send(400, "text/plain", "SSID is required");
    return;
  }
  String ssid = webServer.arg("ssid");
  String password = webServer.arg("password");
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
    webServer.send(400, "text/plain", "Invalid WiFi settings");
    return;
  }
  saveWiFiCredentials(ssid, password);
  webServer.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>"
    "<body><h2>Settings saved</h2><p>The radio will restart and connect to the selected WiFi network.</p></body></html>");
  delay(1200);
  ESP.restart();
}

String makeScanOptions() {
  String html;
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    return "<option value=\"\">No networks found - refresh</option>";
  }
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    String enc = (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "Open" : "Secured";
    html += "<option value=\"" + htmlEscape(ssid) + "\">";
    html += htmlEscape(ssid);
    html += "  (" + String(WiFi.RSSI(i)) + " dBm, " + enc + ")";
    html += "</option>";
  }
  WiFi.scanDelete();
  return html;
}

String htmlEscape(const String& s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  r.replace("'", "&#39;");
  return r;
}

bool loadWiFiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiCreds);
  if (wifiCreds.magic != WIFI_MAGIC ||
      wifiCreds.ssid[0] == '\0' ||
      strlen(wifiCreds.ssid) > 32 ||
      strlen(wifiCreds.password) > 64) {
    return false;
  }
  return true;
}

void saveWiFiCredentials(const String& ssid, const String& password) {
  memset(&wifiCreds, 0, sizeof(wifiCreds));
  wifiCreds.magic = WIFI_MAGIC;
  ssid.toCharArray(wifiCreds.ssid, sizeof(wifiCreds.ssid));
  password.toCharArray(wifiCreds.password, sizeof(wifiCreds.password));
  EEPROM.put(0, wifiCreds);
  EEPROM.commit();
}

// ==========================================================================
//  WiFi connection with proper state management
// ==========================================================================
void connectWiFi() {
  if (!loadWiFiCredentials()) {
    startSetupPortal();
    return;
  }

  wifiFailAttempts = 0;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    WiFi.disconnect();
    delay(300);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiCreds.ssid, wifiCreds.password);

    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(0, 0);
    tft.print("WiFi attempt ");
    tft.print(attempt);
    tft.print("/3");
    tft.setCursor(0, 16);
    tft.print(wifiCreds.ssid);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
      delay(250);
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      // Success - disable setup portal if active
      if (setupPortalActive) {
        setupPortalActive = false;
        if (!webServerStarted) {
          setupWebServer();
        }
      }
      tft.setCursor(0, 36);
      tft.print("WiFi connected");
      tft.setCursor(0, 50);
      tft.print(WiFi.localIP());
      delay(900);
      return;
    }

    tft.setCursor(0, 70);
    tft.print("Connection failed");
    delay(500);
  }

  // All attempts failed - start setup portal
  if (!setupPortalActive) {
    startSetupPortal();
  }
}

// ==========================================================================
//  Main web server setup
// ==========================================================================
void setupWebServer() {
  if (webServerStarted) {
    webServer.stop();
    webServerStarted = false;
  }

  webServer.on("/", HTTP_GET, webHandleRoot);
  webServer.on("/connect", HTTP_POST, webHandleConnect);
  webServer.on("/stations", HTTP_GET, handleStationList);
  webServer.on("/stations/add", HTTP_POST, handleAddStation);
  webServer.on("/stations/delete", HTTP_POST, handleDeleteStation);
  webServer.on("/stations/update", HTTP_POST, handleUpdateStation);
  webServer.on("/stations/reset", HTTP_POST, handleResetStations);
  webServer.on("/manage", HTTP_GET, handleManagePage);
  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Not Found");
  });

  webServer.begin();
  webServerStarted = true;
}

// ==========================================================================
//  Encoder - CORRECTED direction
// ==========================================================================
void initEncoder() {
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT);
  pinMode(ENC_SW, INPUT_PULLUP);
  encLastState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  encAccumulator = 0;
}

void handleEncoderStep() {
  static uint8_t lastState = 0;
  static int8_t quarterSteps = 0;
  static unsigned long lastMove = 0;

  uint8_t clk = digitalRead(ENC_CLK) ? 1 : 0;
  uint8_t dt  = digitalRead(ENC_DT)  ? 1 : 0;
  uint8_t state = (clk << 1) | dt;

  if (lastState == 0) {
    lastState = state;
    return;
  }

  static const int8_t table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };

  uint8_t index = (lastState << 2) | state;
  quarterSteps += table[index];
  lastState = state;

  int dir = 0;
  if (quarterSteps >= 2) {
    quarterSteps = 0;
    if (millis() - lastMove > 40) {
      lastMove = millis();
      dir = +1;
    }
  } else if (quarterSteps <= -2) {
    quarterSteps = 0;
    if (millis() - lastMove > 40) {
      lastMove = millis();
      dir = -1;
    }
  }

  if (dir != 0) {
    // If direction is reversed, uncomment the next line:
    // dir = -dir;
    onEncoderStep(dir);
  }
}

// ==========================================================================
//  Menu functions
// ==========================================================================
void onEncoderStep(int dir) {
  if (!menuActive) {
    openMenu();
  }
  moveMenuHighlight(dir);
}

void openMenu() {
  menuActive = true;
  menuHighlight = currentStation;
  menuScrollOffset = 0;
  menuLastActivity = millis();
  adjustMenuScroll();
  drawMenu();
}

void moveMenuHighlight(int dir) {
  menuHighlight += dir;
  if (menuHighlight < 0) menuHighlight = stationCount - 1;
  if (menuHighlight >= stationCount) menuHighlight = 0;
  menuLastActivity = millis();
  adjustMenuScroll();
  drawMenu();
}

void adjustMenuScroll() {
  if (menuHighlight < menuScrollOffset) {
    menuScrollOffset = menuHighlight;
  } else if (menuHighlight >= menuScrollOffset + MENU_VISIBLE_ROWS) {
    menuScrollOffset = menuHighlight - MENU_VISIBLE_ROWS + 1;
  }
  if (menuScrollOffset < 0) menuScrollOffset = 0;
  int maxOffset = stationCount - MENU_VISIBLE_ROWS;
  if (maxOffset < 0) maxOffset = 0;
  if (menuScrollOffset > maxOffset) menuScrollOffset = maxOffset;
}

void drawMenu() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(2, 3);
  tft.print("Select station");

  int last = min(stationCount, menuScrollOffset + MENU_VISIBLE_ROWS);
  for (int i = menuScrollOffset; i < last; i++) {
    int row = i - menuScrollOffset;
    int y = MENU_TOP_Y + row * MENU_ROW_H;
    bool hl = (i == menuHighlight);

    tft.fillRect(0, y, tft.width(), MENU_ROW_H - 1, hl ? ST77XX_WHITE : ST77XX_BLACK);

    uint16_t fg = hl ? ST77XX_BLACK : (i == currentStation ? ST77XX_YELLOW : ST77XX_WHITE);
    tft.setTextColor(fg);
    tft.setCursor(4, y + 2);
    tft.print(i == currentStation ? ">" : " ");
    tft.print(i + 1);
    tft.print(". ");
    String nm = stations[i].name;
    if (nm.length() > 22) nm = nm.substring(0, 22);
    tft.print(nm);
  }
}

void closeMenu(bool selectStation) {
  menuActive = false;
  drawStaticUI();

  if (selectStation && menuHighlight != currentStation) {
    currentStation = menuHighlight;
    connectToStation(currentStation);
  } else {
    updateStationUI();
  }

  updateVolumeUI();
  updateClockUI();
}

// ==========================================================================
//  Metadata callback
// ==========================================================================
void metadataCB(void *cbData, const char *type, bool isUnicode, const char *value) {
  (void)cbData; (void)isUnicode;
  char t[32];
  char v[96];
  strncpy_P(t, type, sizeof(t) - 1);  t[sizeof(t) - 1] = 0;
  strncpy_P(v, value, sizeof(v) - 1); v[sizeof(v) - 1] = 0;

  if (strcmp(t, "StreamTitle") == 0) {
    String np = String(v);
    if (np != nowPlaying) {
      nowPlaying = np;
      nowPlayingChanged = true;
      connectionError = false;
    }
  }
}

float volumeToGain(int vol) {
  return (vol / 100.0f) * 2.0f;
}

// ==========================================================================
//  Display functions - optimized to reduce flicker
// ==========================================================================
void drawWifiSignal() {
  int rssi = WiFi.RSSI();
  if (abs(rssi - lastRSSI) < 3 && millis() - lastWifiUpdate < 5000) {
    return;
  }
  lastRSSI = rssi;
  lastWifiUpdate = millis();

  int bars = 0;
  if (rssi > -50) bars = 4;
  else if (rssi > -65) bars = 3;
  else if (rssi > -75) bars = 2;
  else if (rssi > -85) bars = 1;
  else bars = 0;

  int x = tft.width() - 30;
  int y = 2;
  int w = 5;
  int gap = 2;

  tft.fillRect(x-2, y, 35, 12, ST77XX_BLACK);

  for (int i = 0; i < 4; i++) {
    int h = 2 + (i * 2);
    if (i < bars) {
      tft.fillRect(x + i*(w+gap), y + (10 - h), w, h, ST77XX_GREEN);
    } else {
      tft.drawRect(x + i*(w+gap), y + (10 - h), w, h, ST77XX_ORANGE);
    }
  }
}

void drawStaticUI() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawFastHLine(0, 16, tft.width(), ST77XX_WHITE);
  tft.drawFastHLine(0, 94, tft.width(), ST77XX_WHITE);
}

void updateClockUI() {
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(2, 3);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  tft.print(buf);
  tft.setCursor(55, 3);
  tft.print(persianDateStr);
}

void updateStationUI() {
  tft.fillRect(0, 20, tft.width(), 34, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 22);
  tft.print(currentStation + 1);
  tft.print("/");
  tft.print(stationCount);
  tft.print("  ");
  tft.print(stations[currentStation].name);

  tft.setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft.setCursor(2, 40);
  String np = nowPlaying;
  if (connectionError) {
    np = "Connection failed!";
    tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
  } else if (np.length() == 0) {
    np = muted ? "(Muted)" : "Connecting ...";
  }
  if (np.length() > 26) np = np.substring(0, 26);
  tft.print(np);
}

void updateVolumeUI() {
  tft.fillRect(0, 100, tft.width(), 22, ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 103);
  tft.print("Vol");
  int barW = map(volume, 0, 100, 0, 110);
  tft.drawRect(28, 102, 110, 10, ST77XX_WHITE);
  tft.fillRect(28, 102, barW, 10, muted ? ST77XX_RED : ST77XX_GREEN);
}

// ==========================================================================
//  Audio control
// ==========================================================================
void stopAudio() {
  if (audioMP3)  { audioMP3->stop();  delete audioMP3;  audioMP3  = nullptr; }
  if (audioBuff) { audioBuff->close(); delete audioBuff; audioBuff = nullptr; }
  if (audioFile) { audioFile->close(); delete audioFile; audioFile = nullptr; }
}

void connectToStation(int idx) {
  stopAudio();
  nowPlaying = "";
  nowPlayingChanged = true;
  connectionError = false;
  updateStationUI();

  audioFile = new AudioFileSourceICYStream(stations[idx].url.c_str());
  audioFile->RegisterMetadataCB(metadataCB, (void*)"ICY");
  audioBuff = new AudioFileSourceBuffer(audioFile, AUDIO_BUFFER_SIZE);
  audioMP3  = new AudioGeneratorMP3();

  if (!audioMP3->begin(audioBuff, audioOut)) {
    delete audioMP3;
    audioMP3 = nullptr;
    delete audioBuff;
    audioBuff = nullptr;
    delete audioFile;
    audioFile = nullptr;
    connectionError = true;
    updateStationUI();
    return;
  }
  connectionError = false;
}

// ==========================================================================
//  Button and volume
// ==========================================================================
void handleButton() {
  static bool lastState = HIGH;
  static unsigned long lastDebounce = 0;

  bool state = digitalRead(ENC_SW);
  if (state != lastState && millis() - lastDebounce > 200) {
    lastDebounce = millis();
    if (state == LOW) {
      if (menuActive) {
        closeMenu(true);
      } else {
        muted = !muted;
        if (audioOut) audioOut->SetGain(muted ? 0.0f : volumeToGain(volume));
        updateVolumeUI();
        updateStationUI();
      }
    }
  }
  lastState = state;
}

void handleVolume() {
  static unsigned long lastRead = 0;
  if (millis() - lastRead < 150) return;
  lastRead = millis();

  int raw = analogRead(VOL_PIN);
  int vol = map(raw, 0, 1023, 0, 100);

  if (abs(vol - volume) >= 2) {
    volume = vol;
    if (!muted && audioOut) audioOut->SetGain(volumeToGain(volume));
    if (!menuActive) updateVolumeUI();
  }
}

// ==========================================================================
//  NTP
// ==========================================================================
uint32_t getCurrentLocalEpoch() {
  if (!timeSynced) return 0;
  return syncedEpochLocal + (uint32_t)((millis() - syncedMillis) / 1000UL);
}

bool syncNTP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (!ntpUDP.begin(NTP_LOCAL_PORT)) {
    return false;
  }

  uint8_t packetBuffer[48];
  for (uint8_t serverIndex = 0; serverIndex < NTP_SERVER_COUNT; serverIndex++) {
    IPAddress serverIP;
    if (WiFi.hostByName(ntpServers[serverIndex], serverIP) != 1) {
      continue;
    }

    memset(packetBuffer, 0, sizeof(packetBuffer));
    packetBuffer[0] = 0b11100011;
    packetBuffer[1] = 0;
    packetBuffer[2] = 6;
    packetBuffer[3] = 0xEC;

    ntpUDP.beginPacket(serverIP, 123);
    ntpUDP.write(packetBuffer, sizeof(packetBuffer));
    ntpUDP.endPacket();

    unsigned long start = millis();
    while (millis() - start < 3000) {
      int size = ntpUDP.parsePacket();
      if (size >= 48) {
        ntpUDP.read(packetBuffer, sizeof(packetBuffer));
        uint32_t secsSince1900 =
          ((uint32_t)packetBuffer[40] << 24) |
          ((uint32_t)packetBuffer[41] << 16) |
          ((uint32_t)packetBuffer[42] << 8)  |
          (uint32_t)packetBuffer[43];

        if (secsSince1900 > 2208988800UL) {
          uint32_t utcEpoch = secsSince1900 - 2208988800UL;
          syncedEpochLocal = utcEpoch + IRAN_OFFSET;
          syncedMillis = millis();
          timeSynced = true;
          lastNtpSync = millis();
          ntpUDP.stop();
          return true;
        }
      }
      delay(10);
      yield();
    }
  }

  ntpUDP.stop();
  return false;
}

void updateDateTime() {
  uint32_t localEpoch = getCurrentLocalEpoch();
  if (localEpoch == 0) return;

  time_t now = (time_t)localEpoch;
  struct tm *t = gmtime(&now);
  if (!t) return;

  hh = t->tm_hour;
  mm = t->tm_min;
  ss = t->tm_sec;

  pd.setGregorianDate(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  pd.convertGregorianToPersian();
  persianDateStr = pd.getPersianDateString();
}

// ==========================================================================
//  Setup
// ==========================================================================
void setup() {
  // Initialize SPI with higher speed for display
  SPI.begin();
  SPI.setFrequency(20000000);  // 20 MHz
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawStaticUI();

  // Initialize SPIFFS and load station list
  if (!SPIFFS.begin()) {
    resetToDefaultStations();
  } else {
    if (!loadStationsFromSPIFFS()) {
      resetToDefaultStations();
    }
    SPIFFS.end();
  }

  connectWiFi();

  if (setupPortalActive) {
    return;
  }

  // Setup web server for normal operation
  setupWebServer();

  // NTP sync
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.print("Syncing time...");
  if (syncNTP()) {
    updateDateTime();
    tft.setCursor(0, 14);
    tft.print("Time OK");
  } else {
    tft.setCursor(0, 14);
    tft.print("Time not available");
  }
  delay(700);

  audioOut = new AudioOutputI2S();
  audioOut->SetGain(volumeToGain(volume));

  drawStaticUI();
  connectToStation(currentStation);

  initEncoder();
}

// ==========================================================================
//  Loop
// ==========================================================================
void loop() {
  if (setupPortalActive) {
    handleSetupPortal();
    yield();
    delay(2);
    return;
  }

  // Handle web server
  webServer.handleClient();
  yield();

  // Timeout for web requests (if browser hangs)
  if (webRequestActive && millis() - webRequestStartTime > 8000) {
    webRequestActive = false;
    resumePlayback();
  }

  handleEncoderStep();
  handleButton();
  handleVolume();

  if (menuActive && millis() - menuLastActivity > MENU_TIMEOUT) {
    closeMenu(false);
  }

  if (audioMP3 && audioMP3->isRunning()) {
    if (!audioMP3->loop()) {
      audioMP3->stop();
      delay(1000);
      connectToStation(currentStation);
    }
  } else if (audioMP3) {
    delay(1000);
    connectToStation(currentStation);
  }

  if (timeSynced && millis() - lastNtpSync > NTP_INTERVAL) {
    syncNTP();
  }

  static unsigned long lastClock = 0;
  if (millis() - lastClock > 1000) {
    lastClock = millis();
    updateDateTime();
    if (!menuActive) {
      updateClockUI();
      drawWifiSignal();
    }
  }

  if (nowPlayingChanged && !menuActive) {
    updateStationUI();
    nowPlayingChanged = false;
  }
}
