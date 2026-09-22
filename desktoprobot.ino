/*
 * ESP32-S3 Desktop Robot Alarm Clock
 *
 * Normal mode shows the time. At boot the robot shows animated eyes.
 * GPIO 7 is a capacitive touch input:
 *   - first tap: blink + alien sound
 *   - second tap: smiling eyes + second sound
 *   - long touch: love eyes + love sound
 * GPIO 1 enters/advances alarm setup, GPIO 2 increments, GPIO 3 decrements
 * (or stops the alarm while it is sounding).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/i2s_std.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 8
#define OLED_SCL 9
#define OLED_ADDRESS 0x3C
#define TOUCH_PIN 7
#define BUTTON_SET 1
#define BUTTON_UP 2
#define BUTTON_STOP 3
#define I2S_BCLK 4
#define I2S_LRC 5
#define I2S_DOUT 6
#define SAMPLE_RATE 22050

const char *WIFI_SSID = "ideanet";
const char *WIFI_PASSWORD = "CHANGE_ME";
const long GMT_OFFSET_SEC = 19800;
const int DAYLIGHT_OFFSET_SEC = 0;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
i2s_chan_handle_t tx_handle = nullptr;
bool i2sEnabled = false;

int alarmHour = 7, alarmMinute = 0;
bool alarmSet = false;
volatile bool alarmPlaying = false;
bool alarmTriggered = false;

enum SettingMode { NORMAL_MODE, SET_HOUR, SET_MINUTE };
SettingMode settingMode = NORMAL_MODE;
bool lastSetState = HIGH, lastUpState = HIGH, lastStopState = HIGH;
unsigned long lastButtonTime = 0;

uint16_t touchThreshold = 0;
bool touchWasDown = false;
unsigned long touchStartedAt = 0;
unsigned long lastTapAt = 0;
byte tapCount = 0;

#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_A4 440

const int melody[] = {
  NOTE_C4, NOTE_C4, NOTE_G4, NOTE_G4, NOTE_A4, NOTE_A4, NOTE_G4,
  NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_D4, NOTE_C4,
  NOTE_G4, NOTE_G4, NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4,
  NOTE_G4, NOTE_G4, NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4,
  NOTE_C4, NOTE_C4, NOTE_G4, NOTE_G4, NOTE_A4, NOTE_A4, NOTE_G4,
  NOTE_F4, NOTE_F4, NOTE_E4, NOTE_E4, NOTE_D4, NOTE_D4, NOTE_C4
};
const int noteDuration[] = {
  400,400,400,400,400,400,700, 400,400,400,400,400,400,700,
  400,400,400,400,400,400,700, 400,400,400,400,400,400,700,
  400,400,400,400,400,400,700, 400,400,400,400,400,400,900
};
const int melodyLength = sizeof(melody) / sizeof(melody[0]);

bool initializeI2S() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &tx_handle, nullptr) != ESP_OK) return false;
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_BCLK,
      .ws = (gpio_num_t)I2S_LRC, .dout = (gpio_num_t)I2S_DOUT,
      .din = I2S_GPIO_UNUSED, .invert_flags = { false, false, false }
    }
  };
  if (i2s_channel_init_std_mode(tx_handle, &cfg) != ESP_OK) return false;
  i2s_channel_enable(tx_handle);
  i2s_channel_disable(tx_handle);
  i2sEnabled = false;
  return true;
}

void startAudio() {
  if (tx_handle && !i2sEnabled && i2s_channel_enable(tx_handle) == ESP_OK) i2sEnabled = true;
}

void stopAudio() {
  if (tx_handle && i2sEnabled) {
    i2s_channel_disable(tx_handle);
    i2sEnabled = false;
  }
}

bool playTone(int frequency, int durationMs, bool watchStop = false) {
  if (!tx_handle) return false;
  int remaining = SAMPLE_RATE * durationMs / 1000;
  int16_t buffer[128];
  float phase = 0.0f;
  float step = 2.0f * PI * frequency / SAMPLE_RATE;
  while (remaining > 0) {
    if (watchStop && digitalRead(BUTTON_STOP) == LOW) return false;
    int count = min(remaining, 128);
    for (int i = 0; i < count; ++i) {
      buffer[i] = (int16_t)(sinf(phase) * 32767.0f * 0.72f);
      phase += step;
      if (phase >= 2.0f * PI) phase -= 2.0f * PI;
    }
    size_t written = 0;
    i2s_channel_write(tx_handle, buffer, count * sizeof(int16_t), &written, 20);
    remaining -= count;
  }
  return true;
}

void playEffect(int first, int second, int third, int length = 110) {
  startAudio();
  playTone(first, length);
  playTone(second, length);
  if (third > 0) playTone(third, length + 30);
  stopAudio();
}

bool playMelodyOnce() {
  for (int i = 0; i < melodyLength && alarmPlaying; ++i) {
    if (!playTone(melody[i], noteDuration[i], true)) return false;
    unsigned long until = millis() + 35;
    while (millis() < until) {
      if (digitalRead(BUTTON_STOP) == LOW) return false;
      delay(2);
    }
  }
  return alarmPlaying;
}

void drawFace(const char *mood, bool blink = false) {
  display.clearDisplay();
  int leftX = 38, rightX = 90, y = 30;
  if (blink) {
    display.drawLine(leftX - 10, y, leftX + 10, y, SSD1306_WHITE);
    display.drawLine(rightX - 10, y, rightX + 10, y, SSD1306_WHITE);
  } else if (!strcmp(mood, "smile")) {
    display.fillCircle(leftX, y - 2, 12, SSD1306_WHITE);
    display.fillCircle(rightX, y - 2, 12, SSD1306_WHITE);
    display.fillCircle(leftX, y - 6, 5, SSD1306_BLACK);
    display.fillCircle(rightX, y - 6, 5, SSD1306_BLACK);

    // Adafruit_GFX does not provide drawArc(). Draw a small pixel-art
    // smile instead, using only APIs available in all library versions.
    display.drawLine(54, 40, 58, 44, SSD1306_WHITE);
    display.drawLine(58, 44, 64, 46, SSD1306_WHITE);
    display.drawLine(64, 46, 70, 44, SSD1306_WHITE);
    display.drawLine(70, 44, 74, 40, SSD1306_WHITE);
  } else if (!strcmp(mood, "love")) {
    display.fillTriangle(leftX - 11, y - 4, leftX, y + 10, leftX + 11, y - 4, SSD1306_WHITE);
    display.fillCircle(leftX - 6, y - 5, 6, SSD1306_WHITE);
    display.fillCircle(leftX + 6, y - 5, 6, SSD1306_WHITE);
    display.fillTriangle(rightX - 11, y - 4, rightX, y + 10, rightX + 11, y - 4, SSD1306_WHITE);
    display.fillCircle(rightX - 6, y - 5, 6, SSD1306_WHITE);
    display.fillCircle(rightX + 6, y - 5, 6, SSD1306_WHITE);
  } else {
    display.fillRoundRect(leftX - 13, y - 14, 26, 28, 8, SSD1306_WHITE);
    display.fillRoundRect(rightX - 13, y - 14, 26, 28, 8, SSD1306_WHITE);
    display.fillCircle(leftX + 2, y - 3, 5, SSD1306_BLACK);
    display.fillCircle(rightX + 2, y - 3, 5, SSD1306_BLACK);
  }
  display.display();
}

void showClock() {
  struct tm t;
  if (!getLocalTime(&t)) {
    display.clearDisplay(); display.setTextSize(1); display.setCursor(24, 28);
    display.setTextColor(SSD1306_WHITE); display.println("SYNCING TIME..."); display.display(); return;
  }
  char text[6]; strftime(text, sizeof(text), "%H:%M", &t);
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE); display.setTextSize(4);
  int16_t x, y; uint16_t w, h; display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2); display.println(text); display.display();
}

void faceForTouch(const char *mood, bool blink, int a, int b, int c) {
  drawFace(mood, blink);
  playEffect(a, b, c);
  delay(180);
  showClock();
}

void showSettingScreen() {
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor(35, 2); display.println("SET ALARM");
  char text[8]; snprintf(text, sizeof(text), "%02d:%02d", alarmHour, alarmMinute);
  display.setTextSize(3); display.setCursor(25, 20); display.println(text);
  display.setTextSize(1); display.setCursor(settingMode == SET_HOUR ? 27 : 63, 51);
  display.println(settingMode == SET_HOUR ? "^ HOUR" : "^ MIN"); display.display();
}

bool pressed(int pin, bool &last, unsigned long debounce = 160) {
  bool now = digitalRead(pin);
  bool hit = now == LOW && last == HIGH && millis() - lastButtonTime > debounce;
  if (hit) lastButtonTime = millis();
  last = now;
  return hit;
}

void checkButtons() {
  if (alarmPlaying) {
    if (pressed(BUTTON_STOP, lastStopState, 80)) {
      alarmPlaying = false; stopAudio(); alarmSet = false; alarmTriggered = true; showClock();
    }
    return;
  }
  if (pressed(BUTTON_SET, lastSetState)) {
    if (settingMode == NORMAL_MODE) settingMode = SET_HOUR;
    else if (settingMode == SET_HOUR) settingMode = SET_MINUTE;
    else { settingMode = NORMAL_MODE; alarmSet = true; alarmTriggered = false; }
    if (settingMode == NORMAL_MODE) { delay(700); showClock(); }
    else showSettingScreen();
    return;
  }
  if (pressed(BUTTON_UP, lastUpState)) {
    if (settingMode == SET_HOUR) alarmHour = (alarmHour + 1) % 24;
    else if (settingMode == SET_MINUTE) alarmMinute = (alarmMinute + 1) % 60;
    if (settingMode != NORMAL_MODE) showSettingScreen();
  }
  if (pressed(BUTTON_STOP, lastStopState)) {
    if (settingMode == SET_HOUR) alarmHour = (alarmHour + 23) % 24;
    else if (settingMode == SET_MINUTE) alarmMinute = (alarmMinute + 59) % 60;
    if (settingMode != NORMAL_MODE) showSettingScreen();
  }
}

void checkTouch() {
  bool down = touchRead(TOUCH_PIN) < touchThreshold;
  if (down && !touchWasDown) {
    touchStartedAt = millis();
    if (lastTapAt && millis() - lastTapAt < 1500) tapCount = 2; else tapCount = 1;
    lastTapAt = millis();
    if (tapCount == 1) faceForTouch("normal", true, 880, 1175, 1568);
    else faceForTouch("smile", false, 523, 659, 784);
  }
  if (down && millis() - touchStartedAt > 900 && tapCount != 3) {
    tapCount = 3; lastTapAt = 0; faceForTouch("love", false, 659, 784, 988);
  }
  if (!down && touchWasDown && tapCount == 3) tapCount = 0;
  touchWasDown = down;
  if (tapCount == 2 && millis() - lastTapAt > 1600) tapCount = 0;
}

void checkAlarm() {
  if (!alarmSet || alarmPlaying) return;
  struct tm t; if (!getLocalTime(&t)) return;
  if (t.tm_hour == alarmHour && t.tm_min == alarmMinute && t.tm_sec < 2 && !alarmTriggered) {
    alarmTriggered = true; alarmPlaying = true; startAudio();
  }
  if (t.tm_min != alarmMinute) alarmTriggered = false;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; ++i) delay(500);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_SET, INPUT_PULLUP); pinMode(BUTTON_UP, INPUT_PULLUP); pinMode(BUTTON_STOP, INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) while (true) delay(100);
  initializeI2S();
  drawFace("normal"); delay(500); drawFace("normal", true); delay(180); drawFace("normal"); delay(900);
  connectWiFi(); configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "time.google.com");
  struct tm t; getLocalTime(&t, 10000);
  uint32_t total = 0; for (int i = 0; i < 16; ++i) { total += touchRead(TOUCH_PIN); delay(20); }
  touchThreshold = max((uint16_t)10, (uint16_t)((total / 16) * 0.70f));
  showClock();
}

void loop() {
  if (alarmPlaying) {
    if (!playMelodyOnce()) { alarmPlaying = false; stopAudio(); showClock(); }
    return;
  }
  checkButtons(); checkTouch(); checkAlarm();
  static unsigned long lastClock = 0;
  if (settingMode == NORMAL_MODE && millis() - lastClock > 500) { showClock(); lastClock = millis(); }
  delay(5);
}
