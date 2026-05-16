/*
 * =====================================================
 *  CHALO SAFAR AI - Vehicle Safety System
 *  ESP32-CAM (AI Thinker) + HLK-LD2410B + DFPlayer Mini
 * =====================================================
 *
 *  WIRING:
 *  -------
 *  RADAR (HLK-LD2410B):
 *    VCC → 5V  |  GND → GND  |  TX → GPIO13
 *
 *  DFPLAYER MINI:
 *    VCC → 5V  |  GND → GND
 *    RX  → GPIO14 (via 1KΩ resistor)
 *    TX  → GPIO15
 *    SPK+ / SPK- → Speaker
 *
 *  SD CARD files (root of SD card, named exactly):
 *    0001.mp3 → Caution audio
 *    0002.mp3 → Danger audio
 *    0003.mp3 → Obstacle very close audio
 *
 *  RISK LEVELS (laptop decides based on radar distance):
 *    Track 1 = Caution    (distance > 200 cm)
 *    Track 2 = Danger     (distance 80–200 cm)
 *    Track 3 = Very Close (distance < 80 cm)
 *
 *  Audio plays ONLY when ML model detects low visibility.
 *  No audio when clear.
 *
 *  HTTP ENDPOINTS:
 *    GET /stream        → MJPEG stream (port 81)
 *    GET /radar         → Radar JSON data
 *    GET /play?track=N  → Play track N (1/2/3)
 *    GET /stop          → Stop audio
 * =====================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
// #include <SoftwareSerial.h>
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ===========================
// WiFi
// ===========================
const char* ssid     = "KRISHVENIGALLA1138";
const char* password = "3x,827J8";

// ===========================
// RADAR - UART1
// ===========================
#define RADAR_RX_PIN  13
#define RADAR_TX_PIN  15   // dummy, not connected
#define RADAR_BAUD    256000

HardwareSerial radarSerial(1);

struct RadarData {
  bool     targetDetected;
  uint16_t movingDistance;
  uint8_t  movingEnergy;
  uint16_t staticDistance;
  uint8_t  staticEnergy;
  uint16_t detectionDistance;
  unsigned long lastUpdate;
};
RadarData radarData = {false,0,0,0,0,0,0};

uint8_t radarBuf[64];
int     radarBufPos = 0;

// ===========================
// DFPLAYER - UART2
// ===========================
#define DFPLAYER_TX_PIN  14
#define DFPLAYER_RX_PIN  -1
#define DFPLAYER_BAUD    9600

HardwareSerial      dfSerial(2);
DFRobotDFPlayerMini dfPlayer;

bool          dfPlayerReady = false;
bool          isPlaying     = false;
unsigned long lastPlayTime  = 0;
#define MIN_PLAY_INTERVAL 4000

// ===========================
// Forward declarations
// ===========================
void startCameraServer();
void readRadar();
void parseRadarFrame(uint8_t* frame, int len);
String getRadarJSON();
bool playTrack(int track);
void stopAudio();

// ===========================
// SETUP
// ===========================
void setup() {
  setCpuFrequencyMhz(160);
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n=== CHALO SAFAR AI - Booting ===");

  // Radar UART1
  radarSerial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  delay(500); // Give the radar half a second to start its "Auto-Broadcast"
  // Serial.println("[RADAR] Started on GPIO13 at 256000 Baud");
  Serial.println("[RADAR] RX on GPIO13, Baud 256000");
  Serial.println("[RADAR] Started on GPIO13");

  // DFPlayer UART2
  dfSerial.begin(DFPLAYER_BAUD, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  delay(3000);

  if (dfPlayer.begin(dfSerial,false)) {
    dfPlayerReady = true;
    dfPlayer.volume(25);
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    delay(2000);
    dfPlayer.playMp3Folder(1);
    Serial.println("[DFPLAYER] Ready! Volume=25");
  } else {
    // dfPlayerReady = false;
    Serial.println("[DFPLAYER] ERROR - Check wiring & SD card!");
  }

  // Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_240X240;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAMERA] Init failed: 0x%x\n", err);
    return;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  Serial.println("[CAMERA] Ready!");

  // WiFi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  startCameraServer();

  Serial.println("\n========= ENDPOINTS =========");
  Serial.printf("  Stream  : http://%s:81/stream\n",     WiFi.localIP().toString().c_str());
  Serial.printf("  Radar   : http://%s/radar\n",         WiFi.localIP().toString().c_str());
  Serial.printf("  Caution : http://%s/play?track=1\n",  WiFi.localIP().toString().c_str());
  Serial.printf("  Danger  : http://%s/play?track=2\n",  WiFi.localIP().toString().c_str());
  Serial.printf("  VClose  : http://%s/play?track=3\n",  WiFi.localIP().toString().c_str());
  Serial.printf("  Stop    : http://%s/stop\n",          WiFi.localIP().toString().c_str());
  Serial.println("=============================\n");
}


void loop() {
  readRadar();

  static int lastState = -1;

  int dist = radarData.detectionDistance;
  int state = -1;

  // classify distance
  if (dist < 50) state = 3;        // very close
  else if (dist < 100) state = 2;   // danger
  else if (dist < 150) state = 1;   // caution
  else state = 0;                   // safe

  // ONLY CHANGE AUDIO IF STATE CHANGES
  if (state != lastState && millis() - lastPlayTime > 1500) {

    if (state == 3) {
      playTrack(3);
      Serial.println("[STATE] VERY CLOSE → TRACK 3");
    }
    else if (state == 2) {
      playTrack(2);
      Serial.println("[STATE] DANGER → TRACK 2");
    }
    else if (state == 1) {
      playTrack(1);
      Serial.println("[STATE] CAUTION → TRACK 1");
    }
    else {
      stopAudio();
      Serial.println("[STATE] SAFE → STOP");
    }

    lastState = state;
  }

  // DFPlayer status
  if (dfPlayerReady && dfPlayer.available()) {
    if (dfPlayer.readType() == DFPlayerPlayFinished) {
      isPlaying = false;
    }
  }

  delay(20);
}

void readRadar() {
  while (radarSerial.available()) {
    uint8_t b = radarSerial.read();

    if (radarBufPos < 64) {
      radarBuf[radarBufPos++] = b;
    } else {
      radarBufPos = 0;
    }

    // Look for full frame: HEADER + FOOTER
    if (radarBufPos >= 20) {

      // Check HEADER
      if (radarBuf[0]==0xF4 && radarBuf[1]==0xF3 &&
          radarBuf[2]==0xF2 && radarBuf[3]==0xF1) {

        // Check FOOTER at end
        int fp = radarBufPos - 4;
        if (radarBuf[fp]==0xF8 && radarBuf[fp+1]==0xF7 &&
            radarBuf[fp+2]==0xF6 && radarBuf[fp+3]==0xF5) {

          parseRadarFrame(radarBuf, radarBufPos);
          radarBufPos = 0; // reset for next frame
        }
      } else {
        // Shift buffer (realign if header lost)
        memmove(radarBuf, radarBuf + 1, radarBufPos - 1);
        radarBufPos--;
      }
    }
  }
}

void parseRadarFrame(uint8_t* frame, int len) {
  if (len < 13) return;

  uint8_t* d = frame + 6;

  radarData.targetDetected = (d[1] == 0xAA);

  // Distance (MOVING)
  uint16_t dist = d[3] | (d[4] << 8);

  // 🚫 Strong garbage filter
  if (dist == 0 || dist > 500) return;

  // OPTIONAL: filter sudden spikes
  static uint16_t lastGood = 0;
  if (abs((int)dist - (int)lastGood) > 200) {
    dist = lastGood; // ignore spike
  }
  lastGood = dist;

  radarData.movingDistance    = dist;
  radarData.detectionDistance = dist;

  // STATIC (keep but ignore if nonsense)
  uint16_t stat = d[5] | (d[6] << 8);
  if (stat < 10000) {   // crude sanity filter
    radarData.staticDistance = stat;
  }

  radarData.lastUpdate = millis();
}

String getRadarJSON() {

  // 🔒 Make a safe copy
  RadarData safeData = radarData;

  char buf[300];
  snprintf(buf, sizeof(buf),
    "{"
      "\"target_detected\":%s,"
      "\"detection_distance_cm\":%d,"
      "\"moving_distance_cm\":%d,"
      "\"moving_energy\":%d,"
      "\"static_distance_cm\":%d,"
      "\"static_energy\":%d,"
      "\"data_age_ms\":%lu"
    "}",
    safeData.targetDetected ? "true" : "false",
    safeData.detectionDistance,
    safeData.movingDistance,
    safeData.movingEnergy,
    safeData.staticDistance,
    safeData.staticEnergy,
    millis() - safeData.lastUpdate
  );

  return String(buf);
}
bool playTrack(int track) {
  if (!dfPlayerReady) return false;
  if (track < 1 || track > 3) return false;

  if (isPlaying) {
    dfPlayer.stop();
    delay(100);
  }

  dfPlayer.play(track);
  isPlaying = true;
  lastPlayTime = millis();

  Serial.printf("[DFPLAYER] Playing track %d\n", track);
  return true;
}

void stopAudio() {
  if (dfPlayerReady) {
    dfPlayer.stop();
    isPlaying = false;
    Serial.println("[DFPLAYER] Stopped");
  }
}
