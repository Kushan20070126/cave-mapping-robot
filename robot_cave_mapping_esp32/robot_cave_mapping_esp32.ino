#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>

// ============================================================
// CONFIG
// ============================================================
const char* SSID = "swifi";
const char* PASS = "123456784";

// Your laptop's LAN IP — get it with `hostname -I` on Linux
IPAddress   LAPTOP_IP(10, 151, 173, 234);
const int   LAPTOP_PORT = 4210;   // video destination port on laptop

// ============================================================
// LED PINS (AI-Thinker ESP32-CAM)
// ============================================================
#define LED_FLASH_PIN  4      // white flash LED (active LOW)
#define LED_STATUS_PIN 33     // red status LED   (active LOW)

const int CTRL_PORT = 4211;   // laptop sends LED commands HERE
WiFiUDP ctrlUdp;
WiFiUDP udp;

// ============================================================
// CAMERA PIN MAP (AI-Thinker)
// ============================================================
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

static camera_config_t cam_cfg = {
  .pin_pwdn = PWDN_GPIO_NUM, .pin_reset = RESET_GPIO_NUM,
  .pin_xclk = XCLK_GPIO_NUM, .pin_sscb_sda = SIOD_GPIO_NUM, .pin_sscb_scl = SIOC_GPIO_NUM,
  .pin_d7 = Y9_GPIO_NUM, .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM, .pin_d4 = Y6_GPIO_NUM,
  .pin_d3 = Y5_GPIO_NUM, .pin_d2 = Y4_GPIO_NUM, .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM,
  .pin_vsync = VSYNC_GPIO_NUM, .pin_href = HREF_GPIO_NUM, .pin_pclk = PCLK_GPIO_NUM,
  .xclk_freq_hz = 24000000,
  .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
  .pixel_format = PIXFORMAT_JPEG,
  .frame_size   = FRAMESIZE_QVGA,       // 320x240 — real-time sweet spot
  .jpeg_quality = 10,                   // 0=best, 63=worst. 10 ≈ 8 KB/frame
  .fb_count     = 2,                    // double buffering
  .fb_location  = CAMERA_FB_IN_PSRAM,
  .grab_mode    = CAMERA_GRAB_LATEST
};

uint16_t frame_id = 0;

#define MAX_PAYLOAD 1400
#define HDR_SIZE    4

// ============================================================
// LED HELPERS
// ============================================================
bool flashState = false;

void setFlash(bool on) {
  flashState = on;
  digitalWrite(LED_FLASH_PIN, on ? LOW : HIGH);   // active LOW
}

void setStatusLed(bool on) {
  digitalWrite(LED_STATUS_PIN, on ? LOW : HIGH);  // active LOW
}

// ============================================================
// LED CONTROL HANDLER (UDP :4211)
// ============================================================
void handleControl() {
  int sz = ctrlUdp.parsePacket();
  if (sz <= 0) return;
  char buf[32] = {0};
  int n = ctrlUdp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;

  // Trim trailing whitespace/newlines
  while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
    buf[--n] = 0;

  Serial.printf("[CTRL] \"%s\"\n", buf);

  if      (!strcmp(buf, "LED_ON"))     setFlash(true);
  else if (!strcmp(buf, "LED_OFF"))    setFlash(false);
  else if (!strcmp(buf, "LED_TOGGLE")) setFlash(!flashState);
  else if (!strcmp(buf, "STATUS_ON"))  setStatusLed(true);
  else if (!strcmp(buf, "STATUS_OFF")) setStatusLed(false);
  else    Serial.println("[CTRL] unknown command");
}

// ============================================================
// SETUP — WiFi first, camera second, with full diagnostics
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("====================");
  Serial.println("ESP32-CAM Booting...");
  Serial.println("====================");

  // ---- LED init ----
  pinMode(LED_FLASH_PIN, OUTPUT);
  pinMode(LED_STATUS_PIN, OUTPUT);
  setFlash(false);
  setStatusLed(false);

  // ============================================================
  // STEP 1 — WiFi FIRST so we get an IP even if camera fails
  // ============================================================
  Serial.print("[1/2] Connecting to WiFi \"");
  Serial.print(SSID);
  Serial.println("\"...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // critical for low latency
  WiFi.setTxPower(WIFI_POWER_19_5dBm);     // max TX power
  WiFi.begin(SSID, PASS);

  unsigned long wifiStart = millis();
  const unsigned long WIFI_TIMEOUT = 30000;   // 30 s

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > WIFI_TIMEOUT) {
      Serial.println();
      Serial.println("!!! WiFi TIMEOUT after 30s");
      Serial.print  ("    Status code: ");
      Serial.println((int)WiFi.status());
      Serial.println("    Status meanings:");
      Serial.println("      0 = IDLE");
      Serial.println("      1 = NO_SSID_AVAIL   (2.4GHz? SSID spelling?)");
      Serial.println("      3 = CONNECTED");
      Serial.println("      4 = CONNECT_FAILED  (wrong password?)");
      Serial.println("      6 = DISCONNECTED");
      Serial.println("    >> Blinking RED LED slowly to signal failure <<");
      while (true) {
        setStatusLed(true);  delay(400);
        setStatusLed(false); delay(400);
      }
    }
  }

  Serial.println();
  Serial.println("========================================");
  Serial.print  ("  >>> ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.print  ("  >>> RSSI: ");
  Serial.print  (WiFi.RSSI());
  Serial.println(" dBm");
  Serial.println("========================================");
  Serial.printf ("  Streaming to %s:%d\n",
                 LAPTOP_IP.toString().c_str(), LAPTOP_PORT);
  Serial.printf ("  Control on UDP port %d\n", CTRL_PORT);
  Serial.println("========================================");

  // ---- Start UDP sockets ----
  udp.begin(4212);           // video TX (any free local port)
  ctrlUdp.begin(CTRL_PORT);  // control RX

  // ============================================================
  // STEP 2 — Camera init (WiFi already up regardless of outcome)
  // ============================================================
  Serial.print("[2/2] Initializing camera... ");

  esp_err_t err = esp_camera_init(&cam_cfg);
  if (err != ESP_OK) {
    Serial.println("FAILED");
    Serial.printf ("      Error code: 0x%x\n", err);
    Serial.println("      Common causes:");
    Serial.println("        - Not enough power (needs 5V/2A + 100uF cap)");
    Serial.println("        - Wrong board selected (AI Thinker)");
    Serial.println("        - GPIO0 not tied to GND during upload");
    Serial.println("        - Loose OV2640 ribbon cable");
    Serial.println("      >> WiFi is UP but camera is DEAD <<");
    Serial.println("      >> You can still ping this device <<");
    Serial.println("      >> Blinking RED LED in 2-fast / 1-slow pattern <<");
    while (true) {
      setStatusLed(true);  delay(100); setStatusLed(false); delay(100);
      setStatusLed(true);  delay(100); setStatusLed(false); delay(100);
      setStatusLed(true);  delay(600); setStatusLed(false); delay(600);
    }
  }
  Serial.println("OK");

  // ---- Sensor tuning for low latency ----
  sensor_t* s = esp_camera_sensor_get();
  s->set_whitebal(s, 0);        // manual WB — no auto-adaptation lag
  s->set_awb_gain(s, 0);
  s->set_exposure_ctrl(s, 1);   // AEC on...
  s->set_aec2(s, 0);            // ...but no frame-rate-dropping AEC2
  s->set_gain_ctrl(s, 1);
  s->set_brightness(s, 0);
  s->set_ae_level(s, 0);
  s->set_aec_value(s, 300);     // cap exposure time

  // ---- 3 quick blinks = fully ready ----
  for (int i = 0; i < 3; ++i) {
    setStatusLed(true);  delay(80);
    setStatusLed(false); delay(80);
  }

  Serial.println("=== READY — streaming starting ===");
  Serial.println();
}

// ============================================================
// LOOP — stream + handle control
// ============================================================
void loop() {
  // Handle LED control packets
  handleControl();

  // Grab a frame
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    delay(50);      // brief yield if camera is dead — don't spin
    return;
  }

  const size_t len = fb->len;
  const uint8_t total = (len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;

  // Safety: skip absurd frames (shouldn't happen at QVGA/q10)
  if (total == 0 || total > 250) {
    esp_camera_fb_return(fb);
    return;
  }

  uint8_t pkt[HDR_SIZE + MAX_PAYLOAD];
  pkt[0] = (frame_id >> 8) & 0xFF;
  pkt[1] =  frame_id       & 0xFF;
  pkt[3] = total;

  // Send all fragments back-to-back with no delay
  for (uint8_t i = 0; i < total; ++i) {
    size_t off   = (size_t)i * MAX_PAYLOAD;
    size_t chunk = (len - off) < MAX_PAYLOAD ? (len - off) : MAX_PAYLOAD;
    pkt[2] = i;
    memcpy(pkt + HDR_SIZE, fb->buf + off, chunk);
    udp.beginPacket(LAPTOP_IP, LAPTOP_PORT);
    udp.write(pkt, chunk + HDR_SIZE);
    udp.endPacket();
  }

  frame_id++;
  esp_camera_fb_return(fb);
}