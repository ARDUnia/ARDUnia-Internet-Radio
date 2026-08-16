/*
 * ============================================================================
 *                         ARDUnia Internet Radio
 * ============================================================================
 *
 *  Project     : ARDUnia Internet Radio
 *  Version     : 2.0.0
 *  Release     : Stable
 *  Copyright   : (c) 2026 ARDUnia / Hamidreza Milaninia
 *
 *  Repository  : https://github.com/ARDUnia/ARDUnia-Internet-Radio
 *
 * ----------------------------------------------------------------------------
 *  DESCRIPTION
 * ----------------------------------------------------------------------------
 *
 *  An open-source Internet Radio receiver based on the ESP8266 NodeMCU.
 *
 *  The project receives Internet radio streams over Wi-Fi, performs software
 *  MP3 decoding on the ESP8266, and outputs digital audio through the I2S
 *  interface to a MAX98357A digital amplifier.
 *
 * ----------------------------------------------------------------------------
 *  FEATURES
 * ----------------------------------------------------------------------------
 *
 *  - Internet radio streaming over Wi-Fi
 *  - Software MP3 decoding using ESP8266Audio
 *  - I2S digital audio output
 *  - ST7735S 160x128 color TFT display
 *  - Rotary encoder for station selection
 *  - Rotary encoder push button for mute
 *  - Analog potentiometer for volume control
 *  - ICY / StreamTitle metadata support
 *  - NTP clock synchronization
 *  - Persian (Jalali) date display
 *  - Wi-Fi configuration through an onboard web server
 *  - Wi-Fi credentials stored in EEPROM
 *  - Startup memory management menu
 *  - Memory reset option
 *  - 90-minute charging mode
 *  - Charging animation and remaining-time display
 *  - Charge-complete audio notification
 *  - Automatic radio stream reconnection
 *  - Optimized display updates to reduce flicker
 *  - Buffer and volume status indicators
 *
 * ----------------------------------------------------------------------------
 *  HARDWARE
 * ----------------------------------------------------------------------------
 *
 *  Microcontroller : ESP8266 NodeMCU/WeMos D1 Mini
 *  Display         : ST7735S 160x128 TFT
 *  Audio           : MAX98357A I2S Digital Amplifier
 *  Encoder         : Rotary Encoder Module
 *  Volume          : Analog Potentiometer
 *  Charger Module  : TP4056/D1 Mini Battery Shield 
 *  Battery         : 3.7V Li-Ion / Li-Po
 *
 * ----------------------------------------------------------------------------
 *  IMPORTANT
 * ----------------------------------------------------------------------------
 *
 *  The charging mode implemented in this project is a software timer and
 *  user-interface feature only. The ESP8266 does not measure battery voltage,
 *  charging current, battery temperature, or actual state of charge.
 *
 *  A suitable dedicated Li-Ion/Li-Po battery charger and protection circuit
 *  must be used for safe battery charging.
 *
 * ----------------------------------------------------------------------------
 *  REQUIRED LIBRARIES
 * ----------------------------------------------------------------------------
 *
 *  - ESP8266Audio
 *  - Adafruit GFX Library
 *  - Adafruit ST7735 and ST7789 Library
 *  - PersianDate
 *
 * ----------------------------------------------------------------------------
 *  LICENSE
 * ----------------------------------------------------------------------------
 *
 *  This project is released under the MIT License.
 *
 *  See the LICENSE file in this repository for the complete license text.
 *
 * ============================================================================
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
// V6.4: buffer memory is reserved statically to avoid heap allocation/fragmentation.
static uint8_t audioBuffer[AUDIO_BUFFER_SIZE];

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

// V6.4: actual fill level of the pre-allocated audio buffer is displayed.
uint8_t displayedBufferPercent = 0;

// -------------------------- Ultra-light 8-band visual equalizer --------------------------
// V5.8: Visual-only analyzer. The audio/I2S path is never delayed by spectrum work.
// Audio samples are decimated only for the analyzer; the original samples are
// always passed unchanged to AudioOutputI2S.
//
// Analyzer:
//   - 32-sample window at 1/16 of the audio sample rate
//   - one Goertzel band per loop pass
//   - integer-only arithmetic
//   - no float / sqrt / cos / int64
//   - analyzer refresh is slow enough to keep CPU/SPI available for audio

volatile uint8_t eqLevels[8] = {0,0,0,0,0,0,0,0};
volatile uint8_t eqPeaks[8]  = {0,0,0,0,0,0,0,0};

class EqualizerAudioOutput : public AudioOutputI2S {
public:
  EqualizerAudioOutput() : AudioOutputI2S() {
    resetAnalyzer();
  }

  bool SetRate(int hz) override {
    bool ok = AudioOutputI2S::SetRate(hz);
    resetAnalyzer();
    return ok;
  }

  bool ConsumeSample(int16_t sample[2]) override {
    // Audio output ALWAYS comes first. Nothing here is allowed to interfere
    // with the I2S timing.
    bool ok = AudioOutputI2S::ConsumeSample(sample);

    // Only every 16th sample is copied for the visual analyzer.
    // The audio stream itself is NOT decimated.
    if (++decimateCounter >= 16) {
      decimateCounter = 0;
      int16_t mono = (int16_t)(((int32_t)sample[0] + (int32_t)sample[1]) >> 1);
      sampleBuffer[writePos] = mono;
      writePos = (writePos + 1) & (BLOCK_SIZE - 1);
    }

    return ok;
  }

  void process() {
    unsigned long now = millis();

    // Take a new snapshot only after the previous 8-band frame is complete.
    if (!analyzing) {
      if ((unsigned long)(now - lastSnapshot) < 140UL) return;

      uint8_t wp = writePos;
      for (uint8_t i = 0; i < BLOCK_SIZE; i++) {
        workBuffer[i] = sampleBuffer[(wp + i) & (BLOCK_SIZE - 1)];
      }

      analyzing = true;
      analysisBand = 0;
      lastBandProcess = now;
      lastSnapshot = now;
      return;
    }

    // One band only. This keeps each loop pass extremely short.
    if ((unsigned long)(now - lastBandProcess) < 10UL) return;

    calculateBand(analysisBand);
    analysisBand++;

    lastBandProcess = now;
    if (analysisBand >= 8) {
      analyzing = false;
    }
  }

private:
  static const uint8_t BLOCK_SIZE = 32;

  int16_t sampleBuffer[BLOCK_SIZE];
  int16_t workBuffer[BLOCK_SIZE];

  volatile uint8_t writePos = 0;
  uint8_t decimateCounter = 0;
  uint8_t analysisBand = 0;

  bool analyzing = false;
  unsigned long lastSnapshot = 0;
  unsigned long lastBandProcess = 0;

  // Q14 Goertzel coefficients.
  // Analyzer sample rate is approximately 44.1kHz / 16 = 2756Hz.
  // Bands are intentionally spread over the audible range.
  static const int16_t coeff[8];

  void resetAnalyzer() {
    writePos = 0;
    decimateCounter = 0;
    analysisBand = 0;
    analyzing = false;
    lastSnapshot = millis();
    lastBandProcess = millis();

    for (uint8_t i = 0; i < 8; i++) {
      eqLevels[i] = 0;
      eqPeaks[i] = 0;
    }

    for (uint8_t i = 0; i < BLOCK_SIZE; i++) {
      sampleBuffer[i] = 0;
      workBuffer[i] = 0;
    }
  }

  void calculateBand(uint8_t b) {
    // Find DC level. This is only 32 additions.
    int32_t sum = 0;
    for (uint8_t i = 0; i < BLOCK_SIZE; i++) {
      sum += workBuffer[i];
    }
    int16_t dc = (int16_t)(sum >> 5);

    int32_t s1 = 0;
    int32_t s2 = 0;
    const int16_t c = coeff[b];

    for (uint8_t n = 0; n < BLOCK_SIZE; n++) {
      int32_t x = (int32_t)workBuffer[n] - dc;

      // Q14 coefficient. All values remain safely inside int32.
      int32_t s0 = x + (((int32_t)c * s1) >> 14) - s2;

      // Safety clamp.
      if (s0 > 1000000000L) s0 = 1000000000L;
      else if (s0 < -1000000000L) s0 = -1000000000L;

      s2 = s1;
      s1 = s0;
    }

    // A cheap magnitude estimate. No squaring and no sqrt.
    uint32_t a = (s1 < 0) ? (uint32_t)(-s1) : (uint32_t)s1;
    uint32_t d = (s2 < 0) ? (uint32_t)(-s2) : (uint32_t)s2;
    uint32_t magnitude = (a + d) >> 12;

    // Convert magnitude to a visual 0..100 level.
    // These thresholds are deliberately lower than V5.7 so normal music
    // produces visible bars.
    uint8_t level;
    if      (magnitude >= 700) level = 100;
    else if (magnitude >= 520) level = 90;
    else if (magnitude >= 380) level = 78;
    else if (magnitude >= 270) level = 66;
    else if (magnitude >= 190) level = 54;
    else if (magnitude >= 125) level = 42;
    else if (magnitude >= 80)  level = 30;
    else if (magnitude >= 45)  level = 20;
    else if (magnitude >= 20)  level = 10;
    else                       level = 2;

    // Fast attack, slow decay.
    uint8_t old = eqLevels[b];

    if (level > old) {
      uint8_t step = (uint8_t)(level - old);
      eqLevels[b] = old + (step > 22 ? 22 : step);
    } else {
      eqLevels[b] = (old > 3) ? (uint8_t)(old - 3) : 0;
    }

    // Peak marker.
    if (eqLevels[b] >= eqPeaks[b]) {
      eqPeaks[b] = eqLevels[b];
    } else if (eqPeaks[b] > 1) {
      eqPeaks[b]--;
    }
  }
};

const int16_t EqualizerAudioOutput::coeff[8] = {
  32138,   // ~ 100 Hz
  30274,   // ~ 180 Hz
  27246,   // ~ 300 Hz
  12540,   // ~ 500 Hz
  -6393,   // ~ 800 Hz
  -30274,  // ~ 1200 Hz
  -27246,  // ~ 1600 Hz
  12540    // ~ 2200 Hz
};

EqualizerAudioOutput *eqAudioOut = nullptr;

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
  {"Radio Yar",            "https://stream-162.zeno.fm/7m8y1x7f8e9uv"},
  {"Iran International",   "http://stream.radiojar.com/iintl_c"},
  {"Radio Liberty",        "http://stream.radiojar.com/cp13r2cpn3quv"},
  {"Radio Hamsafar",       "http://n11.radiojar.com/pyea7q9h5ehvv"},
  {"Radio Faaz",           "http://www.radiofaaz.com:8000/radiofaaz"},
  {"Radio Sarcheshme",     "http://sarcheshmeh.icdndhcp.com:18452/stream"},
  {"Radio Faaz Pop",       "https://free.rcast.net/230792"},
  {"Radio Gachsaran",      "https://radio.gachsaran.org/gach"},
  {"Radio Simorgh",        "https://stream.zeno.fm/9svfnobkrxrvv"},

  {"Radio Swiss Pop",     "http://stream.srg-ssr.ch/m/rsp/mp3_128"},
  {"Classic FM",          "http://media-ice.musicradio.com/ClassicFMMP3"},
  {"WFMU Freeform",        "http://stream0.wfmu.org/freeform-128k"},
  {"Jazz24",               "http://knkx-live-a.edge.audiocdn.com/6285_128k"},
  {"Radio Swiss Jazz",     "http://stream.srg-ssr.ch/srgssr/rsj/mp3/128"},
  {"Radio SRF 1",          "http://stream.srg-ssr.ch/srgssr/srf1/mp3/128"},
  {"HIT RADIO FFH",        "http://mp3.ffh.de/radioffh/hqlivestream.mp3"},
  {"Radio Eins",           "http://rbb-radioeins-live.cast.addradio.de/rbb/radioeins/live/mp3/128/stream.mp3"},
  {"Bremen Eins",          "http://icecast.radiobremen.de/rb/bremeneins/live/mp3/128/stream.mp3"},
  {"Bremen Zwei",          "http://icecast.radiobremen.de/rb/bremenzwei/live/mp3/128/stream.mp3"},
  {"Bremen Vier",          "http://icecast.radiobremen.de/rb/bremenvier/live/mp3/128/stream.mp3"},
  {"Bremen NEXT",          "http://icecast.radiobremen.de/rb/bremennext/live/mp3/128/stream.mp3"},
  {"Shake!FM",             "http://stream.regenbogen.de/shakefm/mp3-128/shakefm"},
  {"1LIVE",                "http://wdr-1live-live.icecast.wdr.de/wdr/1live/live/mp3/128/stream.mp3"},
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
int startupMemoryMenu();
void clearSavedMemory();
void drawEqualizer();

// -------------------------- Charge Mode --------------------------
const uint32_t CHARGE_TOTAL_MS = 90UL * 60UL * 1000UL;
uint32_t chargeRemainingMs = CHARGE_TOTAL_MS;
unsigned long chargeLastTick = 0;
bool chargeModeActive = false;
bool chargeCompleted = false;

void enterChargeMode();
void runChargeMode();
void leaveChargeModeToMenu();
void startNormalOperation();
void playChargeCompleteTone();
void drawChargeScreen();

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
  pinMode(ENC_DT, INPUT_PULLUP);
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

void drawEqualizer() {
  // V5.6: update ONE column per call instead of repainting the whole
  // equalizer. This keeps SPI/TFT activity short enough not to starve audio.
  static uint8_t column = 0;

  const int top = 58;
  const int bottom = 91;
  const int height = bottom - top;
  const int left = 3;
  const int colW = 14;
  const int gap = 2;

  const int x = left + column * (colW + gap);

  // Clear only this column.
  tft.fillRect(x, top, colW, height + 1, ST77XX_BLACK);

  int h = map(eqLevels[column], 0, 100, 0, height - 2);
  if (h < 0) h = 0;
  if (h > height - 2) h = height - 2;

  // Multi-color level zones:
  // green = safe/low, yellow = medium, red = high.
  const int greenLimit = height * 45 / 100;
  const int yellowLimit = height * 78 / 100;

  if (h > 0) {
    int greenH = min(h, greenLimit);
    if (greenH > 0) {
      tft.fillRect(x, bottom - greenH, colW, greenH, ST77XX_GREEN);
    }

    if (h > greenLimit) {
      int yellowH = min(h, yellowLimit) - greenLimit;
      if (yellowH > 0) {
        tft.fillRect(
          x,
          bottom - greenLimit - yellowH,
          colW,
          yellowH,
          ST77XX_YELLOW
        );
      }
    }

    if (h > yellowLimit) {
      int redH = h - yellowLimit;
      if (redH > 0) {
        tft.fillRect(x, bottom - h, colW, redH, ST77XX_RED);
      }
    }
  }

  // White peak marker.
  int peakH = map(eqPeaks[column], 0, 100, 0, height - 2);
  if (peakH > 0 && peakH < height) {
    int peakY = bottom - peakH;
    tft.drawFastHLine(x, peakY, colW, ST77XX_WHITE);
  }

  // Baseline.
  tft.drawFastHLine(x, bottom, colW, ST77XX_WHITE);

  column++;
  if (column >= 8) column = 0;
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

// ========================================================================
// V6.5 Low-impact Volume / Buffer UI
// Only changed pixels are updated during playback.
// ========================================================================
static int8_t lastShownVolume = -1;
static bool lastShownMuted = false;
static int8_t lastShownBuffer = -1;

void drawVolumeFrame() {
  const int x=2, y=99, barY=108, w=76, h=8;
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setCursor(x,y);
  tft.print("VOL");
  tft.drawRect(x,barY,w,h,ST77XX_WHITE);
}

void drawBufferFrame() {
  const int x=82, y=99, barY=108, w=76, h=8;
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(x,y);
  tft.print("BUF");
  tft.drawRect(x,barY,w,h,ST77XX_WHITE);
}

void updateVolumeOnly() {
  const int x=2, y=99, barY=108, w=76, h=8;
  if (volume==lastShownVolume && muted==lastShownMuted) return;

  tft.fillRect(23,y,35,8,ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN,ST77XX_BLACK);
  tft.setCursor(23,y);
  tft.print(volume);
  tft.print("%");

  tft.fillRect(x+1,barY+1,w-2,h-2,ST77XX_BLACK);
  int fill=map(volume,0,100,0,w-2);
  if(fill>0) tft.fillRect(x+1,barY+1,fill,h-2,
                          muted?ST77XX_RED:ST77XX_GREEN);

  lastShownVolume=volume;
  lastShownMuted=muted;
}

void updateBufferOnly() {
  if(!audioBuff) {
    if(lastShownBuffer!=0) {
      tft.fillRect(106,99,48,8,ST77XX_BLACK);
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_RED,ST77XX_BLACK);
      tft.setCursor(106,99);
      tft.print("0%");
      tft.fillRect(83,109,74,6,ST77XX_BLACK);
      lastShownBuffer=0;
    }
    return;
  }

  uint32_t fill=audioBuff->getFillLevel();
  uint8_t pct=(uint8_t)((fill*100UL)/AUDIO_BUFFER_SIZE);
  if(pct>100) pct=100;

  if(lastShownBuffer>=0 && abs((int)pct-(int)lastShownBuffer)<2) return;

  uint16_t c=(pct<15)?ST77XX_RED:((pct<35)?ST77XX_YELLOW:ST77XX_CYAN);

  tft.fillRect(106,99,48,8,ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(c,ST77XX_BLACK);
  tft.setCursor(106,99);
  tft.print(pct);
  tft.print("%");

  const int barX=83, barY=109, barW=74, barH=6;
  int newWidth=map(pct,0,100,0,barW);

  if(lastShownBuffer<0) {
    tft.fillRect(barX,barY,barW,barH,ST77XX_BLACK);
    if(newWidth>0) tft.fillRect(barX,barY,newWidth,barH,c);
  } else {
    int oldWidth=map(lastShownBuffer,0,100,0,barW);
    if(newWidth>oldWidth)
      tft.fillRect(barX+oldWidth,barY,newWidth-oldWidth,barH,c);
    else if(newWidth<oldWidth)
      tft.fillRect(barX+newWidth,barY,oldWidth-newWidth,barH,ST77XX_BLACK);
  }

  displayedBufferPercent=pct;
  lastShownBuffer=pct;
}

void updateVolumeUI() {
  tft.fillRect(0,98,128,28,ST77XX_BLACK);
  drawVolumeFrame();
  drawBufferFrame();
  lastShownVolume=-1;
  lastShownBuffer=-1;
  updateVolumeOnly();
  updateBufferOnly();
}

void updateBufferUI() {
  if(!menuActive) updateBufferOnly();
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
  audioBuff = new AudioFileSourceBuffer(audioFile, audioBuffer, AUDIO_BUFFER_SIZE);
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

// ==========================================================================
//  Startup Memory Menu
//  0 = continue with saved data
//  1 = clear saved WiFi + station list and start fresh
// ==========================================================================
int startupMemoryMenu() {
  initEncoder();

  int selected = 0;
  uint8_t lastState = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  int8_t quarterSteps = 0;
  bool lastButton = HIGH;
  unsigned long lastButtonChange = 0;

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(8, 5);
  tft.println("INTERNET RADIO");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 17);
  tft.println("Startup");

  auto drawStartupMenu = [&]() {
    tft.fillRect(0, 29, 160, 88, ST77XX_BLACK);
    const int y[3] = {30, 58, 86};
    const char* labels[3] = {
      "Continue saved data",
      "Clear memory / fresh",
      "Charge mode (90 min)"
    };

    for (int i = 0; i < 3; i++) {
      if (selected == i) {
        tft.fillRoundRect(3, y[i], 154, 23, 3, ST77XX_CYAN);
        tft.setTextColor(ST77XX_BLACK);
      } else {
        tft.drawRoundRect(3, y[i], 154, 23, 3, ST77XX_CYAN);
        tft.setTextColor(ST77XX_WHITE);
      }
      tft.setCursor(9, y[i] + 8);
      tft.print(labels[i]);
    }

    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(8, 117);
    tft.println("Turn: Select   Press: OK");
  };

  drawStartupMenu();
  const unsigned long STARTUP_MENU_START = millis();
  const unsigned long STARTUP_MENU_TIMEOUT = 7000UL;

  delay(80);
  lastButton = digitalRead(ENC_SW);

  while (true) {
    yield();
    if ((unsigned long)(millis() - STARTUP_MENU_START) >= STARTUP_MENU_TIMEOUT) {
      return 0;
    }

    uint8_t clk = digitalRead(ENC_CLK) ? 1 : 0;
    uint8_t dt  = digitalRead(ENC_DT) ? 1 : 0;
    uint8_t state = (clk << 1) | dt;

    static const int8_t table[16] = {
       0, -1,  1,  0,
       1,  0,  0, -1,
      -1,  0,  0,  1,
       0,  1, -1,  0
    };

    uint8_t index = (lastState << 2) | state;
    quarterSteps += table[index];
    lastState = state;

    if (quarterSteps >= 2) {
      quarterSteps = 0;
      selected++;
      if (selected > 2) selected = 0;
      drawStartupMenu();
    } else if (quarterSteps <= -2) {
      quarterSteps = 0;
      selected--;
      if (selected < 0) selected = 2;
      drawStartupMenu();
    }

    bool button = digitalRead(ENC_SW);
    if (button != lastButton && millis() - lastButtonChange > 180) {
      lastButtonChange = millis();
      lastButton = button;
      if (button == LOW) {
        delay(40);
        while (digitalRead(ENC_SW) == LOW) {
          yield();
          delay(2);
        }
        return selected;
      }
    }
    delay(2);
  }
}

// ==========================================================================
//  Clear all user-saved memory
//  - Removes the saved station file from SPIFFS
//  - Erases the custom WiFi credentials from EEPROM
//  - Clears the ESP8266 WiFi station configuration
// ==========================================================================
void clearSavedMemory() {
  // Clear saved station list.
  if (SPIFFS.begin()) {
    if (SPIFFS.exists("/stations.json")) {
      SPIFFS.remove("/stations.json");
    }
    SPIFFS.end();
  }

  // Clear the custom WiFi credentials stored by this project.
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0xFF);
  }
  EEPROM.commit();
  EEPROM.end();

  memset(&wifiCreds, 0, sizeof(wifiCreds));

  // Also clear the ESP8266 SDK's remembered station configuration.
  WiFi.disconnect(true);
  delay(250);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Reset runtime station state.
  stationCount = 0;
  currentStation = 0;
  nowPlaying = "";
  nowPlayingChanged = true;
}

void startNormalOperation() {
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
  if (setupPortalActive) return;

  setupWebServer();

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

  if (!eqAudioOut) {
    eqAudioOut = new EqualizerAudioOutput();
  }
  audioOut = eqAudioOut;
  audioOut->SetGain(volumeToGain(volume));

  drawStaticUI();
  updateVolumeUI();
  tft.fillRect(0, 58, tft.width(), 34, ST77XX_BLACK);
  tft.drawFastHLine(0, 91, tft.width(), ST77XX_WHITE);

  connectToStation(currentStation);
  initEncoder();
}

void setup() {
  SPI.begin();
  SPI.setFrequency(20000000);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);

  int startupChoice = startupMemoryMenu();

  if (startupChoice == 1) {
    clearSavedMemory();
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(0, 30);
    tft.println("Memory cleared");
    tft.setCursor(0, 48);
    tft.println("Starting fresh...");
    delay(1200);
    startNormalOperation();
  } else if (startupChoice == 2) {
    enterChargeMode();
  } else {
    startNormalOperation();
  }
}

// ========================================================================
// Charge mode - Internet/WiFi/web server/audio playback are disabled.
// The countdown is kept only in RAM, so a power-off starts a new 90-minute
// charge period on the next boot.
// ========================================================================
void enterChargeMode() {
  setupPortalActive = false;
  webRequestActive = false;
  wasPlayingBeforeWeb = false;

  if (webServerStarted) {
    webServer.stop();
    webServerStarted = false;
  }
  stopAudio();
  if (audioOut) {
    audioOut->SetGain(0.0f);
  }

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  ntpUDP.stop();

  chargeModeActive = true;
  chargeCompleted = (chargeRemainingMs == 0);
  chargeLastTick = millis();
  drawChargeScreen();
}

void drawChargeScreen() {
  // Redraw only the portions that actually change.  The previous version
  // used fillScreen() every 500 ms, which caused visible flicker on ST7735S.
  static bool firstDraw = true;
  static uint8_t animation = 0;
  static uint8_t lastPercent = 255;
  static uint32_t lastSeconds = 0xFFFFFFFFUL;

  animation = (animation + 1) % 5;

  const int W = 160;
  const int bx = 55, by = 25, bw = 50, bh = 25;

  if (firstDraw) {
    tft.fillScreen(ST77XX_BLACK);

    // Header
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_CYAN);
    const char *header = "CHARGING";
    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(header, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((W - tw) / 2, 5);
    tft.print(header);

    // Battery outline and terminal
    tft.drawRoundRect(bx, by, bw, bh, 3, ST77XX_WHITE);
    tft.fillRect(bx + bw, by + 7, 4, 11, ST77XX_WHITE);

    // Static instruction
    tft.setTextColor(ST77XX_YELLOW);
    const char *instruction = "Press knob for menu";
    tft.getTextBounds(instruction, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((W - tw) / 2, 88);
    tft.print(instruction);

    // Branding starts at the left edge as requested.
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(0, 112);
    tft.print("ARDUnia Internet Radio");

    firstDraw = false;
  }

  uint8_t percent = (uint8_t)(((CHARGE_TOTAL_MS - chargeRemainingMs) * 100UL) /
                              CHARGE_TOTAL_MS);
  if (percent > 100) percent = 100;
  if (chargeCompleted) percent = 100;

  uint32_t totalSec = chargeRemainingMs / 1000UL;

  // Battery interior: redraw only the 46x21 px dynamic area.
  // This eliminates the full-screen flicker.
  tft.fillRect(bx + 2, by + 2, bw - 4, bh - 4, ST77XX_BLACK);

  int fill = map(percent, 0, 100, 0, bw - 4);
  if (fill > 0) {
    tft.fillRoundRect(bx + 2, by + 2, fill, bh - 4, 2, ST77XX_GREEN);
  }

  // Charging animation is confined to the battery area.
  if (!chargeCompleted) {
    int ax = bx + 5 + animation * 8;
    if (ax + 4 < bx + bw - 2) {
      tft.fillRect(ax, by + 7, 4, 10, ST77XX_YELLOW);
    }
  }

  // Update remaining time only when it actually changes.
  if (totalSec != lastSeconds) {
    tft.fillRect(45, 57, 70, 13, ST77XX_BLACK);

    char timeBuf[12];
    uint16_t mins = totalSec / 60UL;
    uint8_t secs = totalSec % 60UL;
    snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", mins, secs);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(timeBuf, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((W - tw) / 2, 58);
    tft.print(timeBuf);
    lastSeconds = totalSec;
  }

  // Update percentage/status only when necessary.
  if (percent != lastPercent || chargeCompleted) {
    tft.fillRect(35, 71, 90, 14, ST77XX_BLACK);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(1);

    char statusBuf[24];
    if (chargeCompleted) {
      strcpy(statusBuf, "Battery full");
    } else {
      snprintf(statusBuf, sizeof(statusBuf), "%u%% charged", percent);
    }

    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(statusBuf, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((W - tw) / 2, 72);
    tft.print(statusBuf);
    lastPercent = percent;
  }
}

void playChargeCompleteTone() {
  // Generate a short two-tone notification directly through the existing I2S
  // output. No network connection is used.
  if (!audioOut) {
    eqAudioOut = new EqualizerAudioOutput();
    audioOut = eqAudioOut;
  }

  audioOut->SetRate(22050);
  audioOut->SetGain(0.7f);

  const uint32_t sampleRate = 22050UL;
  const uint16_t toneMs[2] = {220, 420};
  const uint16_t freq[2] = {880, 1175};

  for (uint8_t tone = 0; tone < 2; tone++) {
    uint32_t samples = (sampleRate * toneMs[tone]) / 1000UL;
    uint32_t halfPeriod = sampleRate / (freq[tone] * 2UL);
    if (halfPeriod == 0) halfPeriod = 1;
    int16_t sample[2];
    bool high = false;
    uint32_t counter = 0;

    for (uint32_t i = 0; i < samples; i++) {
      if (++counter >= halfPeriod) {
        counter = 0;
        high = !high;
      }
      int16_t v = high ? 6000 : -6000;
      sample[0] = v;
      sample[1] = v;
      audioOut->ConsumeSample(sample);
      if ((i & 127) == 0) yield();
    }
  }
  audioOut->SetGain(0.0f);
}

void leaveChargeModeToMenu() {
  chargeModeActive = false;
  chargeLastTick = millis();
  stopAudio();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  int choice = startupMemoryMenu();
  if (choice == 2) {
    enterChargeMode();
    return;
  }

  if (choice == 1) {
    clearSavedMemory();
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(0, 30);
    tft.println("Memory cleared");
    tft.setCursor(0, 48);
    tft.println("Starting fresh...");
    delay(1200);
  }

  startNormalOperation();
}

void runChargeMode() {
  if (!chargeModeActive) return;

  static bool lastButton = HIGH;
  static unsigned long lastButtonChange = 0;
  bool button = digitalRead(ENC_SW);

  if (button != lastButton && millis() - lastButtonChange > 180) {
    lastButtonChange = millis();
    lastButton = button;
    if (button == LOW) {
      delay(40);
      while (digitalRead(ENC_SW) == LOW) {
        yield();
        delay(2);
      }
      leaveChargeModeToMenu();
      return;
    }
  }

  unsigned long now = millis();
  uint32_t elapsed = (uint32_t)(now - chargeLastTick);
  chargeLastTick = now;

  if (!chargeCompleted) {
    if (elapsed >= chargeRemainingMs) {
      chargeRemainingMs = 0;
      chargeCompleted = true;
      drawChargeScreen();
      playChargeCompleteTone();
    } else {
      chargeRemainingMs -= elapsed;
    }
  }

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= 500UL) {
    lastDraw = millis();
    drawChargeScreen();
  }
  yield();
  delay(2);
}

// ==========================================================================
//  Loop
// ==========================================================================
void loop() {
  if (chargeModeActive) {
    runChargeMode();
    return;
  }

  if (setupPortalActive) {
    handleSetupPortal();
    yield();
    delay(2);
    return;
  }

  // Handle web server
  webServer.handleClient();

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

  static unsigned long lastBufferUI = 0;
  if (!menuActive && millis() - lastBufferUI >= 1200UL) {
    lastBufferUI = millis();
    updateBufferUI();
  }

  // Spectrum processing is deliberately outside the audio sample path.
  // Run the analyzer scheduler at most every 5 ms; the actual audio samples
  // continue to flow continuously through ConsumeSample().
  static unsigned long lastEQProcess = 0;
  if (eqAudioOut && millis() - lastEQProcess >= 5UL) {
    lastEQProcess = millis();
    eqAudioOut->process();
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

  // One cooperative yield per normal loop, rather than multiple yields
  // before audio processing.
  yield();

  // Refresh only one equalizer column at a time.
  // Eight columns therefore complete one visual frame in about 280 ms.
  static unsigned long lastEQDraw = 0;
  if (!menuActive && millis() - lastEQDraw >= 35) {
    lastEQDraw = millis();
    drawEqualizer();
  }
}



