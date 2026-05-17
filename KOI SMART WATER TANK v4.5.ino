/*
 * KOI SMART WATER TANK v4.5 - PASSWORD ISSUE FIXED
 * 
 * v4.5 Critical Fixes:
 * - WiFi password sent via raw HTTP POST (no JSON escape issues)
 * - "Show Password" toggle to verify what you typed
 * - Password length displayed in status
 * - Auto-trim whitespace from SSID/password
 * - Direct ESP-NOW connection method
 * 
 * ACCESS:
 *   WiFi: SmartTank_AP / Pass: 12345678
 *   URL: http://192.168.4.1
 *   Login: jamalmd1
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WebSocketsServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <time.h>

#define BLYNK_PRINT Serial
#define BLYNK_NO_BUILTIN
#define BLYNK_NO_INFO
#include <BlynkSimpleEsp8266.h>

#define PIN_TRIG 12
#define PIN_ECHO 14
#define RELAY_MOTOR1 5
#define RELAY_MOTOR2 4
#define PIN_KILL_SW 0
#define PIN_SDA 2
#define PIN_SCL 13
#define RELAY_ON LOW
#define RELAY_OFF HIGH

#define EEPROM_SIZE 2048
#define EEPROM_MAGIC 0xAB12CD45
#define SONIC_SAMPLES 10
#define SONIC_TRIM_COUNT 2
#define SONIC_READ_INTERVAL 200
#define WS_UPDATE_INTERVAL 1000
#define SENSOR_TIMEOUT_US 30000
#define OLED_TIMEOUT_MS 300000
#define DEBOUNCE_MS 50
#define LEAK_CHECK_WINDOW_MS 3600000UL
#define ANTI_SEIZE_INTERVAL 604800UL
#define ANTI_SEIZE_RUN_TIME 30000
#define DEFAULT_TEMP_C 27.0f
#define DAILY_REBOOT_HOUR 3
#define PULSE_ON_MS 300000UL
#define PULSE_OFF_MS 180000UL
#define MAX_HISTORY_ENTRIES 30
#define HISTORY_PAGE_SIZE 10
#define MAX_SCHEDULES 10

#define AP_SSID "SmartTank_AP"
#define AP_PASS "12345678"
#define AP_CHANNEL 6

#define WIFI_STATE_IDLE 0
#define WIFI_STATE_CONNECTING 1
#define WIFI_STATE_CONNECTED 2
#define WIFI_STATE_FAILED 3

#define WIFI_CONNECT_TIMEOUT 25000
#define WIFI_RECONNECT_DELAY 60000

struct ScheduleSlot {
  int startHour, startMinute, endHour, endMinute;
  bool enabled;
};

struct SystemConfig {
  uint32_t magic;
  float tankHeightCm;
  float tankMaxVolumeLiters;
  int dryRunTimeoutMinutes;
  int maxSingleRunTimeoutMinutes;
  int servoOnLevel;
  int servoOffLevel;
  ScheduleSlot motor2Schedules[MAX_SCHEDULES];
  uint32_t tankCleanElapsedDays;
  int systemOperatingMode;
  float pumpWattage;
  float unitCost;
  char adminPassword[32];
  char blynkAuth[64];
  char wifiSSID[33];
  char wifiPass[65];
  int silentStartHour;
  int silentEndHour;
  uint32_t checksum;
};

struct EventLog {
  char timestamp[40];
  char eventType[40];
  char triggerSource[28];
  float tankLevel;
  bool valid;
};

SystemConfig cfg;
bool cfgDirty = false;

float sonicBuffer[SONIC_SAMPLES];
int sonicIndex = 0;
bool sonicBufferFull = false;
float filteredDistance = 0.0f;
float tankLevelPercent = 0.0f;
float tankVolumeLiters = 0.0f;
unsigned long lastSonicRead = 0;

bool motor1Running = false, motor2Running = false;
bool motor1ManualOverride = false, motor2ManualOverride = false;
unsigned long motor1StartTime = 0, motor2StartTime = 0;
float motor1StartLevel = 0.0f, motor2StartLevel = 0.0f;
unsigned long motor1CumulativeRunSec = 0, motor2CumulativeRunSec = 0;
unsigned long lastRuntimeAccum = 0;

bool dryRunLocked = false;
unsigned long bothMotorsOffSince = 0;
float levelWhenBothOff = 0.0f;
bool leakageAlert = false;
unsigned long motor2LastRunEpoch = 0;
bool antiSeizeRunning = false;
unsigned long antiSeizeStart = 0;
bool pulseMode = false;
bool pulsePhaseOn = true;
unsigned long pulseTimer = 0;
float lastLevelForThrottle = 0.0f;
unsigned long throttleCheckTime = 0;

bool killSwitchActive = false;
unsigned long lastKillDebounce = 0;
int lastKillState = HIGH, killStableState = HIGH;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
unsigned long lastOLEDActivity = 0;
bool oledSleeping = false, oledInitialized = false;
int oledPage = 0;
unsigned long lastOledPageSwitch = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 3600000);

ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
DNSServer dnsServer;

EventLog eventHistory[MAX_HISTORY_ENTRIES];
int eventCount = 0, eventHead = 0;

float hourlyKwh[24];
unsigned long lastPowerCalc = 0, lastWSUpdate = 0;

bool blynkEnabled = false;
unsigned long lastBlynkRun = 0;
unsigned long bootTime = 0;
float ambientTemp = DEFAULT_TEMP_C;
String errorCodes[5];
String wifiScanResult = "{\"networks\":[]}";

int wifiState = WIFI_STATE_IDLE;
unsigned long wifiConnectStart = 0;
unsigned long lastReconnectAttempt = 0;
String wifiStatusMsg = "AP only";
int lastDisconnectReason = 0;

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
WiFiEventHandler wifiGotIPHandler;

void loadConfig();
void saveConfig();
void setDefaults();
uint32_t calcChecksum(SystemConfig* c);
void setMotor1(bool on, const char* source);
void setMotor2(bool on, const char* source);
void addEventLog(const char* eventType, const char* source, float level);
String getFormattedTime();
String getShortTime();
String buildStatusJSON();
String buildHistoryJSON(int page);
String buildConfigJSON();
String buildPowerJSON();
void clearErrors();
void wakeOLED();
void sleepOLED();
void updateOLED();
bool isInMotor2Window();
bool isInSilentHours();
void applyConfigFromJSON(JsonDocument& doc);
void setupWebServer();
void setupBlynk();
void runBlynk();
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void handleWSMessage(uint8_t num, uint8_t* data, size_t len);
void broadcastWSData();
void sendIndexPage();
void startWiFiConnect();
void handleWiFiStateMachine();
void scanWiFiNetworks();
String getDisconnectReason(int reason);

void onWiFiConnected(const WiFiEventStationModeConnected& evt) {
  Serial.printf("[WIFI] Connected to: %s ch%d\n", evt.ssid.c_str(), evt.channel);
  wifiStatusMsg = "Authenticated, getting IP...";
}

void onWiFiGotIP(const WiFiEventStationModeGotIP& evt) {
  Serial.println(F("[WIFI] *** GOT IP! ***"));
  Serial.print(F("[WIFI] IP: ")); Serial.println(evt.ip);
  wifiState = WIFI_STATE_CONNECTED;
  wifiStatusMsg = "Connected: " + evt.ip.toString();
  addEventLog("WiFi Connected", WiFi.SSID().c_str(), tankLevelPercent);
  wakeOLED();
}

void onWiFiDisconnected(const WiFiEventStationModeDisconnected& evt) {
  lastDisconnectReason = evt.reason;
  Serial.printf("[WIFI] Disconnected. Reason: %d (%s)\n", evt.reason, getDisconnectReason(evt.reason).c_str());
  if (wifiState == WIFI_STATE_CONNECTED) {
    wifiState = WIFI_STATE_IDLE;
    wifiStatusMsg = "Disconnected: " + getDisconnectReason(evt.reason);
  }
}

String getDisconnectReason(int reason) {
  switch (reason) {
    case 1: return "Unspecified";
    case 2: return "Auth expired";
    case 6: return "Not authenticated";
    case 7: return "Not associated";
    case 8: return "Disassoc by AP";
    case 15: return "WRONG PASSWORD (handshake fail)";
    case 200: return "Beacon timeout - WiFi out of range";
    case 201: return "AP NOT FOUND - check SSID";
    case 202: return "Auth failed - WRONG PASSWORD";
    case 203: return "Assoc failed";
    case 204: return "Handshake timeout - WRONG PASSWORD";
    default: return "Code " + String(reason);
  }
}

void startWiFiConnect() {
  if (strlen(cfg.wifiSSID) == 0) {
    wifiState = WIFI_STATE_IDLE;
    wifiStatusMsg = "No WiFi configured";
    return;
  }
  
  Serial.println(F("\n[WIFI] === STARTING ==="));
  Serial.printf("[WIFI] SSID: '%s' (len=%d)\n", cfg.wifiSSID, strlen(cfg.wifiSSID));
  Serial.printf("[WIFI] Pass: '%s' (len=%d)\n", cfg.wifiPass, strlen(cfg.wifiPass));
  // Show first/last char of password for verification
  if (strlen(cfg.wifiPass) >= 2) {
    Serial.printf("[WIFI] Pass first char: '%c' (ASCII %d)\n", cfg.wifiPass[0], (int)cfg.wifiPass[0]);
    Serial.printf("[WIFI] Pass last char: '%c' (ASCII %d)\n", cfg.wifiPass[strlen(cfg.wifiPass)-1], (int)cfg.wifiPass[strlen(cfg.wifiPass)-1]);
  }
  
  wifiStatusMsg = "Connecting to " + String(cfg.wifiSSID) + "...";
  wifiState = WIFI_STATE_CONNECTING;
  wifiConnectStart = millis();
  lastDisconnectReason = 0;
  
  WiFi.disconnect(false);
  delay(200);
  WiFi.setAutoReconnect(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);
  
  WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
  Serial.println(F("[WIFI] WiFi.begin() called"));
}

void handleWiFiStateMachine() {
  unsigned long now = millis();
  wl_status_t status = WiFi.status();
  
  switch (wifiState) {
    case WIFI_STATE_IDLE:
      if (strlen(cfg.wifiSSID) > 0 && status != WL_CONNECTED) {
        if (now - lastReconnectAttempt >= WIFI_RECONNECT_DELAY) {
          lastReconnectAttempt = now;
          startWiFiConnect();
        }
      }
      break;
      
    case WIFI_STATE_CONNECTING:
      if (status == WL_CONNECTED) return;
      if (now - wifiConnectStart >= WIFI_CONNECT_TIMEOUT) {
        WiFi.disconnect(false);
        wifiState = WIFI_STATE_FAILED;
        if (lastDisconnectReason == 15 || lastDisconnectReason == 202 || lastDisconnectReason == 204) {
          wifiStatusMsg = "WRONG PASSWORD! Pass len=" + String(strlen(cfg.wifiPass));
        } else if (lastDisconnectReason == 201) {
          wifiStatusMsg = "WiFi NOT FOUND! Check SSID";
        } else if (lastDisconnectReason == 0) {
          wifiStatusMsg = "Timeout. Try: 2.4GHz/WPA2/short pass";
        } else {
          wifiStatusMsg = "Failed: " + getDisconnectReason(lastDisconnectReason);
        }
        Serial.printf("[WIFI] FAILED: %s\n", wifiStatusMsg.c_str());
        lastReconnectAttempt = now;
      }
      break;
      
    case WIFI_STATE_CONNECTED:
      if (status != WL_CONNECTED) {
        wifiState = WIFI_STATE_IDLE;
        wifiStatusMsg = "Disconnected";
      } else {
        wifiStatusMsg = "Connected: " + WiFi.SSID();
      }
      break;
      
    case WIFI_STATE_FAILED:
      if (now - lastReconnectAttempt >= WIFI_RECONNECT_DELAY) {
        if (strlen(cfg.wifiSSID) > 0) startWiFiConnect();
        else wifiState = WIFI_STATE_IDLE;
      }
      break;
  }
}

void scanWiFiNetworks() {
  Serial.println(F("[WIFI] Scanning..."));
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, true);
  StaticJsonDocument<2048> doc;
  JsonArray nets = doc.createNestedArray("networks");
  if (n > 0) {
    int indices[20];
    int count = (n > 20) ? 20 : n;
    for (int i = 0; i < count; i++) indices[i] = i;
    for (int i = 0; i < count - 1; i++) {
      for (int j = 0; j < count - i - 1; j++) {
        if (WiFi.RSSI(indices[j]) < WiFi.RSSI(indices[j+1])) {
          int tmp = indices[j]; indices[j] = indices[j+1]; indices[j+1] = tmp;
        }
      }
    }
    for (int i = 0; i < count; i++) {
      int idx = indices[i];
      String ssid = WiFi.SSID(idx);
      if (ssid.length() == 0) continue;
      JsonObject net = nets.createNestedObject();
      net["ssid"] = ssid;
      net["rssi"] = WiFi.RSSI(idx);
      net["secure"] = (WiFi.encryptionType(idx) != ENC_TYPE_NONE);
      net["ch"] = WiFi.channel(idx);
    }
  }
  wifiScanResult = "";
  serializeJson(doc, wifiScanResult);
  WiFi.scanDelete();
  Serial.printf("[WIFI] Found %d networks\n", n);
}

const char HTML_P1[] PROGMEM = R"KOIPAGE(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>KOI Smart Tank</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg:#0a0e1a;--card:#131833;--card2:#1a2040;--accent:#00b4d8;--green:#00c853;
--red:#ff1744;--orange:#ff9100;--text:#e0e0e0;--text2:#90a4ae;--border:#1e3a5f;--header:#0d1229}
body.light{--bg:#f0f4f8;--card:#fff;--card2:#e8eef5;--accent:#0077b6;--green:#00a040;
--red:#d50000;--orange:#e65100;--text:#1a1a2e;--text2:#5a6878;--border:#cfd8e3;--header:#fff}
body{font-family:'Segoe UI',sans-serif;background:var(--bg);color:var(--text);min-height:100vh;padding-bottom:80px}
.header{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;
background:var(--header);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:100}
.logo{font-size:18px;font-weight:700;color:var(--accent)}
.hdr-right{display:flex;gap:5px;align-items:center}
.mode-btn{background:var(--card2);border:1px solid var(--border);color:var(--green);
padding:5px 10px;border-radius:20px;font-size:10px;cursor:pointer;display:flex;align-items:center;gap:4px}
.dot{width:7px;height:7px;border-radius:50%;background:var(--green)}
.admin-btn,.theme-btn{background:var(--card2);border:1px solid var(--border);color:var(--text2);
padding:5px 8px;border-radius:20px;font-size:10px;cursor:pointer}
.admin-btn.unlocked{color:var(--green);border-color:var(--green)}
.container{padding:10px;max-width:480px;margin:0 auto}
.tank-glass{position:relative;width:140px;height:160px;margin:8px auto;
border:2px solid rgba(0,180,216,.4);border-radius:0 0 14px 14px;border-top:none;
overflow:hidden;background:rgba(10,14,26,.1)}
.water-fill{position:absolute;bottom:0;left:0;right:0;
background:linear-gradient(180deg,rgba(0,180,216,.7),rgba(0,119,182,.85));
transition:height .8s ease;border-radius:0 0 12px 12px}
.tank-text{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);text-align:center;
color:#fff;text-shadow:0 2px 6px rgba(0,0,0,.6)}
body.light .tank-text{color:var(--text)}
.tank-percent{font-size:32px;font-weight:700;line-height:1}
.tank-vol{font-size:11px;opacity:.9}
.tank-max{font-size:9px;opacity:.6}
.health-box{padding:8px;border-radius:8px;background:var(--card);border:1px solid var(--border);margin:6px 0}
.health-title.ok{color:var(--green);font-weight:600;font-size:13px}
.health-title.err{color:var(--red);font-weight:600;font-size:13px}
.health-sub{font-size:10px;color:var(--text2);margin-top:2px}
.pump-status{text-align:center;padding:6px;border-radius:6px;margin:6px 0;font-size:11px;
font-weight:600;border:1px solid var(--border)}
.pump-status.active{background:rgba(0,200,83,.15);color:var(--green);border-color:var(--green)}
.pump-status.idle{background:var(--card);color:var(--text2)}
.metrics-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin:8px 0}
.metric-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:8px}
.metric-label{font-size:9px;color:var(--text2);text-transform:uppercase}
.metric-value{font-size:15px;font-weight:700;margin:2px 0;color:var(--text)}
.metric-sub{font-size:9px;color:var(--text2)}
.progress-bar{height:3px;background:var(--card2);border-radius:2px;margin-top:4px;overflow:hidden}
.progress-fill{height:100%;background:var(--accent)}
.tab-bar{display:flex;gap:2px;margin:10px 0 6px;background:var(--card);
border-radius:6px;padding:2px;border:1px solid var(--border)}
.tab{flex:1;padding:7px 4px;text-align:center;font-size:10px;font-weight:600;cursor:pointer;
border-radius:5px;color:var(--text2)}
.tab.active{background:var(--accent);color:#fff}
.tab-content{display:none}
.tab-content.active{display:block}
.alert-card{background:rgba(255,23,68,.1);border:1px solid rgba(255,23,68,.4);
border-radius:6px;padding:8px;margin:4px 0}
.alert-code{font-weight:700;color:var(--red);font-size:12px}
.alert-msg{font-size:10px;color:var(--text2)}
.chart-box{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:10px;margin:8px 0}
.chart-title{font-size:11px;font-weight:600;margin-bottom:6px;color:var(--text)}
canvas{width:100%;height:100px}
.ctrl-row{display:flex;align-items:center;justify-content:space-between;padding:10px;
background:var(--card);border:1px solid var(--border);border-radius:8px;margin:5px 0}
.ctrl-label{font-size:12px;font-weight:500;color:var(--text)}
.ctrl-sub{font-size:9px;color:var(--text2)}
.toggle{position:relative;width:44px;height:24px;background:var(--card2);border-radius:12px;
cursor:pointer;border:1px solid var(--border)}
.toggle.on{background:var(--green);border-color:var(--green)}
.toggle::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;
border-radius:50%;background:#fff;transition:transform .3s}
.toggle.on::after{transform:translateX(20px)}
.cfg-group{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:10px;margin:6px 0}
.cfg-group-title{font-size:11px;font-weight:600;color:var(--accent);margin-bottom:8px;text-transform:uppercase}
.cfg-row{display:flex;align-items:center;justify-content:space-between;padding:6px 0;
border-bottom:1px solid var(--border)}
.cfg-row:last-child{border:none}
.cfg-label{font-size:11px;color:var(--text2)}
.cfg-input{background:var(--card2);border:1px solid var(--border);color:var(--text);
padding:5px 8px;border-radius:5px;width:90px;text-align:right;font-size:12px}
.btn{padding:8px 16px;border:none;border-radius:6px;font-size:12px;font-weight:600;cursor:pointer}
.btn-primary{background:linear-gradient(135deg,#0077b6,#00b4d8);color:#fff}
.btn-danger{background:rgba(255,23,68,.15);color:var(--red);border:1px solid var(--red)}
.btn-outline{background:transparent;color:var(--accent);border:1px solid var(--accent)}
.btn-sm{padding:5px 10px;font-size:10px}
.bottom-nav{position:fixed;bottom:0;left:0;right:0;display:flex;align-items:center;
justify-content:space-around;padding:6px 0 10px;background:var(--header);
border-top:1px solid var(--border);z-index:100}
.nav-item{display:flex;flex-direction:column;align-items:center;gap:2px;
font-size:9px;color:var(--text2);cursor:pointer;padding:3px 10px}
.nav-item.active{color:var(--accent)}
.stop-btn{width:50px;height:50px;border-radius:50%;background:var(--red);
border:3px solid rgba(255,23,68,.3);color:#fff;font-size:9px;font-weight:700;cursor:pointer;
display:flex;flex-direction:column;align-items:center;justify-content:center;margin-top:-16px}
.modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.7);
z-index:200;align-items:center;justify-content:center}
.modal-overlay.show{display:flex}
.modal{background:var(--card);border:1px solid var(--border);border-radius:12px;
padding:16px;width:90%;max-width:340px;max-height:90vh;overflow-y:auto}
.modal-title{font-size:15px;font-weight:600;margin-bottom:10px;color:var(--text)}
.modal input{width:100%;background:var(--card2);border:1px solid var(--border);
color:var(--text);padding:8px;border-radius:6px;margin-bottom:8px;font-size:13px}
.modal p{font-size:11px;color:var(--text2);margin-bottom:8px;line-height:1.4}
.toast{position:fixed;top:16px;left:50%;transform:translateX(-50%);background:var(--green);
color:#fff;padding:8px 16px;border-radius:6px;font-size:12px;font-weight:500;z-index:300;
opacity:0;transition:opacity .3s;pointer-events:none;max-width:90%;text-align:center}
.toast.show{opacity:1}
.toast.error{background:var(--red)}
.log-entry{padding:6px;border-bottom:1px solid var(--border);font-size:10px}
.log-time{color:var(--accent);font-weight:500}
.log-event{font-weight:600;color:var(--text)}
.log-source{color:var(--text2)}
.log-level{color:var(--orange)}
.more-btn{width:100%;padding:8px;background:var(--card2);border:1px solid var(--border);
color:var(--accent);border-radius:6px;cursor:pointer;font-size:11px;margin-top:6px}
.status-dot{width:6px;height:6px;border-radius:50%;display:inline-block}
.status-dot.on{background:var(--green)}
.status-dot.off{background:var(--text2)}
.sched-card{background:var(--card2);border:1px solid var(--border);border-radius:6px;padding:8px;margin:4px 0}
.sched-row{display:flex;gap:4px;align-items:center;font-size:10px;color:var(--text2);margin:2px 0}
.sched-row input{background:var(--card);border:1px solid var(--border);color:var(--text);
padding:3px 4px;border-radius:3px;width:42px;text-align:center;font-size:11px}
.warn-box{background:rgba(255,145,0,.1);border:1px solid rgba(255,145,0,.4);
border-radius:6px;padding:10px;margin:6px 0;font-size:10px;color:var(--orange);line-height:1.5}
.ap-badge{background:var(--orange);color:#fff;padding:2px 6px;border-radius:10px;font-size:9px;font-weight:600}
.guide-step{background:var(--card2);border-left:3px solid var(--accent);padding:8px;margin:6px 0;border-radius:4px}
.guide-step-num{color:var(--accent);font-weight:700;font-size:11px;margin-bottom:4px}
.guide-code{background:#000;color:#0f0;padding:6px;border-radius:4px;font-family:monospace;
font-size:10px;margin:4px 0;word-break:break-all}
.wifi-item{background:var(--card2);border:1px solid var(--border);border-radius:6px;padding:10px;
margin:4px 0;cursor:pointer;display:flex;justify-content:space-between;align-items:center}
.wifi-item:hover{border-color:var(--accent)}
.wifi-name{font-size:13px;color:var(--text);font-weight:500}
.wifi-rssi{font-size:11px;color:var(--text2)}
.wifi-status-box{padding:12px;border-radius:8px;margin:8px 0;font-size:12px;font-weight:500}
.wifi-status-box.connected{background:rgba(0,200,83,.15);color:var(--green);border:2px solid var(--green)}
.wifi-status-box.connecting{background:rgba(0,180,216,.15);color:var(--accent);border:2px solid var(--accent)}
.wifi-status-box.failed{background:rgba(255,23,68,.15);color:var(--red);border:2px solid var(--red)}
.wifi-status-box.idle{background:var(--card2);color:var(--text2);border:1px solid var(--border)}
.spinner{display:inline-block;width:12px;height:12px;border:2px solid var(--accent);
border-radius:50%;border-top-color:transparent;animation:spin 1s linear infinite;margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.crit-warn{background:#ff1744;color:#fff;padding:10px;border-radius:6px;font-size:11px;
font-weight:600;margin:6px 0;text-align:center}
.wifi-form{background:var(--card2);border-radius:8px;padding:12px;margin:6px 0}
.wifi-form-row{margin:8px 0}
.wifi-form-label{font-size:11px;color:var(--text2);margin-bottom:4px;display:block}
.wifi-form input[type=text],.wifi-form input[type=password]{
width:100%;background:var(--card);border:2px solid var(--border);color:var(--text);
padding:10px;border-radius:6px;font-size:14px;font-family:monospace}
.wifi-form input:focus{border-color:var(--accent);outline:none}
.show-pass-row{display:flex;align-items:center;gap:6px;margin-top:6px;font-size:11px;color:var(--text2);cursor:pointer}
.pass-preview{background:#000;color:#0f0;padding:6px;border-radius:4px;font-family:monospace;
font-size:11px;margin-top:4px;word-break:break-all;min-height:24px}
</style></head><body>
)KOIPAGE";

const char HTML_P2[] PROGMEM = R"KOIPAGE(
<div class="toast" id="toast"></div>

<div class="modal-overlay" id="loginModal"><div class="modal">
<div class="modal-title">Admin Login</div>
<input type="password" id="loginPw" placeholder="Password (default: jamalmd1)">
<div style="display:flex;gap:6px">
<button class="btn btn-primary" style="flex:1" onclick="doLogin()">Login</button>
<button class="btn btn-outline" style="flex:1" onclick="closeM('loginModal')">Cancel</button>
</div></div></div>

<div class="modal-overlay" id="pwModal"><div class="modal">
<div class="modal-title">Change Password</div>
<input type="password" id="oldPw" placeholder="Current password">
<input type="password" id="newPw" placeholder="New password">
<input type="password" id="newPw2" placeholder="Confirm">
<div style="display:flex;gap:6px">
<button class="btn btn-primary" style="flex:1" onclick="doChangePw()">Change</button>
<button class="btn btn-outline" style="flex:1" onclick="closeM('pwModal')">Cancel</button>
</div></div></div>

<div class="modal-overlay" id="resetModal"><div class="modal">
<div class="modal-title" style="color:var(--red)">Factory Reset</div>
<p><strong>WARNING:</strong> Deletes ALL settings.</p>
<input type="password" id="resetPw" placeholder="Type admin password">
<div style="display:flex;gap:6px">
<button class="btn btn-danger" style="flex:1" onclick="doFactoryReset()">RESET</button>
<button class="btn btn-outline" style="flex:1" onclick="closeM('resetModal')">Cancel</button>
</div></div></div>

<div class="header">
<div class="logo">KOI</div>
<button class="mode-btn" onclick="cycleMode()">
<span class="dot" id="modeDot"></span><span id="modeText">AUTO</span>
</button>
<div class="hdr-right">
<span class="ap-badge" id="apBadge">AP</span>
<button class="theme-btn" onclick="toggleTheme()" id="themeBtn">DARK</button>
<button class="admin-btn" id="adminBtn" onclick="toggleAdmin()">
<span id="adminText">Locked</span>
</button>
</div>
</div>

<div class="container">

<div class="tank-glass">
<div class="water-fill" id="waterFill" style="height:0%"></div>
<div class="tank-text">
<div class="tank-percent" id="levelPct">--</div>
<div class="tank-vol"><span id="volNow">--</span> L</div>
<div class="tank-max">of <span id="volMax">--</span> L</div>
</div></div>

<div class="health-box">
<div class="health-title ok" id="healthTitle">System Healthy</div>
<div class="health-sub" id="healthSub">All systems normal</div>
</div>

<div class="pump-status idle" id="pumpStatus">IDLE</div>

<div class="metrics-grid">
<div class="metric-card">
<div class="metric-label">VOLUME</div>
<div class="metric-value"><span id="mVol">--</span> L</div>
<div class="metric-sub">/ <span id="mVolMax">--</span> L</div>
<div class="progress-bar"><div class="progress-fill" id="volBar" style="width:0%"></div></div>
</div>
<div class="metric-card">
<div class="metric-label">TEMP / SOUND</div>
<div class="metric-value"><span id="mTemp">--</span>C</div>
<div class="metric-sub"><span id="mSpeed">--</span> m/s</div>
</div>
<div class="metric-card">
<div class="metric-label">DAILY COST</div>
<div class="metric-value">Rs <span id="mCost">--</span></div>
<div class="metric-sub">/ 24h</div>
</div>
<div class="metric-card">
<div class="metric-label">WIFI</div>
<div class="metric-value"><span id="mRssi">--</span></div>
<div class="metric-sub" id="wifiQual">--</div>
</div>
</div>

<div class="tab-bar">
<div class="tab active" data-tab="dashboard" onclick="switchTab('dashboard')">DASH</div>
<div class="tab" data-tab="control" onclick="switchTab('control')">CTRL</div>
<div class="tab" data-tab="config" onclick="switchTab('config')">CFG</div>
<div class="tab" data-tab="wifi" onclick="switchTab('wifi')">WIFI</div>
<div class="tab" data-tab="guide" onclick="switchTab('guide')">HELP</div>
</div>

<div class="tab-content active" id="tab-dashboard">
<div class="chart-title">ACTIVE ALERTS</div>
<div id="alertList"><div style="font-size:11px;color:var(--text2);padding:8px">No alerts</div></div>
<div class="chart-box">
<div class="chart-title">POWER (24H)</div>
<canvas id="powerChart"></canvas>
</div>
<div class="chart-box">
<div class="chart-title">EVENT HISTORY</div>
<div id="historyList"></div>
<button class="more-btn" id="moreBtn" onclick="loadMoreHist()">Load More</button>
</div>
</div>

<div class="tab-content" id="tab-control">
<div class="ctrl-row">
<div><div class="ctrl-label">Servo Motor</div>
<div class="ctrl-sub"><span id="m1Status" class="status-dot off"></span> <span id="m1Text">CLOSED</span></div></div>
<div class="toggle" id="m1Toggle" onclick="toggleMotor(1)"></div>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label">Motor</div>
<div class="ctrl-sub"><span id="m2Status" class="status-dot off"></span> <span id="m2Text">OFF</span></div></div>
<div class="toggle" id="m2Toggle" onclick="toggleMotor(2)"></div>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label">Mode</div><div class="ctrl-sub" id="ctrlModeText">Automatic</div></div>
<button class="btn btn-sm btn-outline" onclick="cycleMode()">Change</button>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label">Clear Errors</div></div>
<button class="btn btn-sm btn-danger" onclick="clearAll()">Clear</button>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label">Reboot</div></div>
<button class="btn btn-sm btn-outline" onclick="doReboot()">Reboot</button>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label">Password</div></div>
<button class="btn btn-sm btn-outline" onclick="showM('pwModal')">Change</button>
</div>
<div class="ctrl-row">
<div><div class="ctrl-label" style="color:var(--red)">Factory Reset</div></div>
<button class="btn btn-sm btn-danger" onclick="showM('resetModal')">Reset</button>
</div>
</div>

<div class="tab-content" id="tab-config">
<div class="cfg-group">
<div class="cfg-group-title">Tank</div>
<div class="cfg-row"><span class="cfg-label">Height (cm)</span>
<input class="cfg-input" id="c_th" type="number" step="0.1"></div>
<div class="cfg-row"><span class="cfg-label">Max Volume (L)</span>
<input class="cfg-input" id="c_mv" type="number" step="0.1"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Servo Motor</div>
<div class="cfg-row"><span class="cfg-label">OPEN at (%)</span>
<input class="cfg-input" id="c_son" type="number" min="0" max="100"></div>
<div class="cfg-row"><span class="cfg-label">CLOSE below (%)</span>
<input class="cfg-input" id="c_soff" type="number" min="0" max="100"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Schedules</div>
<div id="schedContainer"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Safety</div>
<div class="cfg-row"><span class="cfg-label">Dry Run min</span>
<input class="cfg-input" id="c_dr" type="number" min="0" max="59"></div>
<div class="cfg-row"><span class="cfg-label">Max Run min</span>
<input class="cfg-input" id="c_mr" type="number" min="0" max="599"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Silent Hours</div>
<div class="cfg-row"><span class="cfg-label">Start</span>
<input class="cfg-input" id="c_ss" type="number" min="0" max="23"></div>
<div class="cfg-row"><span class="cfg-label">End</span>
<input class="cfg-input" id="c_se" type="number" min="0" max="23"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Power</div>
<div class="cfg-row"><span class="cfg-label">Watts</span>
<input class="cfg-input" id="c_w" type="number" step="0.1"></div>
<div class="cfg-row"><span class="cfg-label">Rs/kWh</span>
<input class="cfg-input" id="c_uc" type="number" step="0.1"></div>
</div>
<div class="cfg-group">
<div class="cfg-group-title">Blynk</div>
<div class="cfg-row"><span class="cfg-label">Token</span>
<input class="cfg-input" id="c_bt" type="text" style="width:160px"></div>
</div>
<button class="btn btn-primary" style="width:100%;margin:10px 0" onclick="saveCfg()">Save Config</button>
</div>

<div class="tab-content" id="tab-wifi">

<div class="crit-warn">
ESP8266 supports ONLY 2.4GHz + WPA2!<br>
WPA3 / 5GHz will NEVER connect!
</div>

<div class="cfg-group">
<div class="cfg-group-title">WiFi Status</div>
<div class="wifi-status-box idle" id="wifiStatusBox">
<div id="wifiStatusText">Loading...</div>
</div>
<div style="font-size:10px;color:var(--text2);margin-top:6px" id="wifiInfo"></div>
</div>

<div class="cfg-group">
<div class="cfg-group-title">Connect to WiFi (HTTP Form - Reliable)</div>

<form id="wifiForm" class="wifi-form" onsubmit="return submitWifiForm(event)">

<div class="wifi-form-row">
<label class="wifi-form-label">WiFi Name (SSID)</label>
<input type="text" id="wf_ssid" name="ssid" placeholder="Your WiFi name" autocomplete="off" required>
</div>

<div class="wifi-form-row">
<label class="wifi-form-label">WiFi Password</label>
<input type="password" id="wf_pass" name="pass" placeholder="WiFi password" autocomplete="off">

<div class="show-pass-row">
<input type="checkbox" id="showPassChk" onchange="togglePassVisibility()"> 
<label for="showPassChk">Show password (verify what you typed)</label>
</div>

<div class="wifi-form-label" style="margin-top:8px">Password preview:</div>
<div class="pass-preview" id="passPreview">(empty)</div>
<div style="font-size:10px;color:var(--orange);margin-top:4px">
Length: <span id="passLen">0</span> chars | First/last: <span id="passFL">--</span>
</div>
</div>

<button type="submit" class="btn btn-primary" style="width:100%;margin-top:10px">Connect Now</button>
</form>

<div style="font-size:10px;color:var(--text2);margin-top:8px;line-height:1.5">
After clicking Connect, ESP will save and try for 25 seconds. 
Watch the Status box above for live updates.
</div>
</div>

<div class="cfg-group">
<div class="cfg-group-title">Available Networks (Scan)</div>
<button class="btn btn-primary" style="width:100%;margin-bottom:8px" onclick="scanWifi()">Scan WiFi Networks</button>
<div id="wifiList"><div style="font-size:11px;color:var(--text2);padding:6px">Click Scan to find networks. Then click a network to auto-fill SSID above.</div></div>
</div>

<div class="cfg-group">
<div class="cfg-group-title" style="color:var(--red)">Forget WiFi</div>
<button class="btn btn-danger" style="width:100%" onclick="forgetWifi()">Forget Saved WiFi</button>
</div>

</div>

<div class="tab-content" id="tab-guide">

<div class="cfg-group">
<div class="cfg-group-title" style="color:var(--red)">CRITICAL Phone Hotspot Settings</div>
<div style="font-size:11px;color:var(--text2);line-height:1.6">
<div class="guide-step">
<div class="guide-step-num">XIAOMI / MIUI / REDMI</div>
<p>Settings -> Connection and Sharing</p>
<p>-> Personal Hotspot -> Set up</p>
<p>-> Security: <strong>WPA2 Personal</strong></p>
<p>-> Frequency: <strong>2.4 GHz</strong></p>
<p>-> SSID: simple, no spaces (e.g. MyTank)</p>
<p>-> Password: <strong>12345678</strong> (test simple first)</p>
</div>
<div class="guide-step">
<div class="guide-step-num">SAMSUNG</div>
<p>Settings -> Connections -> Mobile Hotspot -> Configure</p>
<p>Security: <strong>WPA2 PSK</strong>, Band: <strong>2.4 GHz</strong></p>
</div>
<div class="guide-step">
<div class="guide-step-num">PIXEL / STOCK ANDROID</div>
<p>Settings -> Network and Internet -> Hotspot</p>
<p>WiFi Hotspot -> Security: WPA2-Personal</p>
<p>AP Band: 2.4 GHz</p>
</div>
<div class="guide-step">
<div class="guide-step-num">iPHONE</div>
<p>Settings -> Personal Hotspot</p>
<p>Enable: <strong>Maximize Compatibility</strong></p>
</div>
</div>
</div>

<div class="cfg-group">
<div class="cfg-group-title" style="color:var(--orange)">If Wrong Password Error</div>
<div style="font-size:11px;color:var(--text2);line-height:1.6">
<div class="guide-step">
<div class="guide-step-num">STEP 1: Verify Password</div>
<p>1. Use "Show Password" checkbox to see exact text</p>
<p>2. Check first and last character displayed</p>
<p>3. Make sure no extra spaces at start/end</p>
<p>4. Caps Lock check (passwords are case-sensitive!)</p>
</div>
<div class="guide-step">
<div class="guide-step-num">STEP 2: Test with simple password</div>
<p>1. Change phone hotspot password to: <strong>12345678</strong></p>
<p>2. Forget WiFi here, reboot ESP</p>
<p>3. Try connecting with new simple password</p>
</div>
<div class="guide-step">
<div class="guide-step-num">STEP 3: Avoid special chars</div>
<p>Special chars like @ # $ % can cause issues.</p>
<p>Use only letters and numbers in password for testing.</p>
</div>
<div class="guide-step">
<div class="guide-step-num">STEP 4: Check Security Type</div>
<p>If your hotspot is WPA3, ESP will fail!</p>
<p>Must be <strong>WPA2-Personal</strong> only.</p>
</div>
</div>
</div>

<div class="cfg-group">
<div class="cfg-group-title" style="color:var(--accent)">Blynk 2.0 Setup</div>
<div style="font-size:11px;color:var(--text2);line-height:1.6">
<div class="guide-step">
<p>1. blynk.cloud -> Sign up</p>
<p>2. Developer Zone -> + New Template -> ESP8266/WiFi</p>
<p>3. Datastreams: V0-V12 (see code comments)</p>
<p>4. Devices -> + New Device -> From Template</p>
<p>5. Copy AUTH TOKEN -> Paste in CFG -> Blynk Token</p>
<p>6. Save -> Reboot -> Done!</p>
</div>
</div>
</div>

</div>

</div>

<div class="bottom-nav">
<div class="nav-item active" onclick="navTo('dashboard')">Home</div>
<div class="nav-item" onclick="navTo('control')">Ctrl</div>
<button class="stop-btn" onclick="eStop()">STOP</button>
<div class="nav-item" onclick="navTo('wifi')">WiFi</div>
<div class="nav-item" onclick="navTo('guide')">Help</div>
</div>
)KOIPAGE";

const char HTML_P3[] PROGMEM = R"KOIPAGE(
<script>
var ws=null,adminPw="",isAdmin=false,histPage=0,data={},powerData=new Array(24).fill(0);
var schedules=[];for(var si=0;si<10;si++)schedules.push({sh:0,sm:0,eh:0,em:0,en:false});
schedules[0]={sh:6,sm:0,eh:8,em:0,en:true};

function loadTheme(){var t=localStorage.getItem("koiTheme")||"dark";
if(t==="light"){document.body.classList.add("light");document.getElementById("themeBtn").textContent="LIGHT";}
else document.getElementById("themeBtn").textContent="DARK";}
function toggleTheme(){if(document.body.classList.contains("light")){document.body.classList.remove("light");
document.getElementById("themeBtn").textContent="DARK";localStorage.setItem("koiTheme","dark");}
else{document.body.classList.add("light");document.getElementById("themeBtn").textContent="LIGHT";
localStorage.setItem("koiTheme","light");}}

function connectWS(){ws=new WebSocket("ws://"+location.hostname+":81/");
ws.onopen=function(){showT("Connected","ok")};
ws.onmessage=function(e){try{var d=JSON.parse(e.data);
if(d.auth!==undefined){if(d.auth==="ok"){isAdmin=true;adminPw=document.getElementById("loginPw").value;
closeM("loginModal");updAdmin();showT("Admin OK","ok");loadCfg();}else showT("Wrong password","err");return;}
if(d.status==="ok"){if(d.msg)showT(d.msg,"ok");return;}
if(d.status==="rebooting"){showT("Rebooting","ok");return;}
if(d.error){showT(d.error,"err");return;}
if(d.events!==undefined){renderHist(d);return;}
if(d.hourly!==undefined){powerData=d.hourly;return;}
if(d.networks!==undefined){renderWifiList(d.networks);return;}
if(d.tankHeightCm!==undefined){fillCfg(d);return;}
data=d;updUI();updWifiStatus();}catch(ex){console.error(ex);}};
ws.onclose=function(){setTimeout(connectWS,3000);};}

function wsSend(o){if(!ws||ws.readyState!==1){showT("Not connected","err");return;}
if(adminPw)o.password=adminPw;ws.send(JSON.stringify(o));}

function updUI(){var d=data,lvl=d.level||0;
document.getElementById("levelPct").textContent=Math.round(lvl)+"%";
document.getElementById("waterFill").style.height=lvl+"%";
document.getElementById("volNow").textContent=Math.round(d.volume||0);
document.getElementById("volMax").textContent=Math.round(d.maxVolume||0);
document.getElementById("mVol").textContent=Math.round(d.volume||0);
document.getElementById("mVolMax").textContent=Math.round(d.maxVolume||0);
document.getElementById("volBar").style.width=lvl+"%";
document.getElementById("mTemp").textContent=(d.temp||27).toFixed(1);
document.getElementById("mSpeed").textContent=(d.soundSpeed||347).toFixed(1);
document.getElementById("mCost").textContent=(d.dailyCost||0).toFixed(2);
document.getElementById("mRssi").textContent=d.wifiConnected?(d.rssi||0)+" dBm":"AP only";
var rssi=d.rssi||0,wq=document.getElementById("wifiQual");
if(d.wifiConnected){wq.textContent=rssi>-50?"Excellent":rssi>-60?"Good":rssi>-70?"Fair":"Poor";}
else wq.textContent="No internet";
var m1=d.motor1,m2=d.motor2;
document.getElementById("m1Toggle").className="toggle"+(m1?" on":"");
document.getElementById("m2Toggle").className="toggle"+(m2?" on":"");
document.getElementById("m1Status").className="status-dot "+(m1?"on":"off");
document.getElementById("m2Status").className="status-dot "+(m2?"on":"off");
document.getElementById("m1Text").textContent=m1?"OPEN":"CLOSED";
document.getElementById("m2Text").textContent=m2?"ON":"OFF";
var ps=document.getElementById("pumpStatus");
if(m1||m2){ps.className="pump-status active";var s=[];if(m1)s.push("Servo OPEN");if(m2)s.push("Motor ON");
ps.innerHTML=s.join(" - ");}else{ps.className="pump-status idle";ps.innerHTML="ALL IDLE";}
var modes=["AUTO","MANUAL","HOLIDAY"],colors=["var(--green)","var(--orange)","var(--accent)"];
document.getElementById("modeText").textContent=modes[d.mode||0];
document.getElementById("modeDot").style.background=colors[d.mode||0];
document.getElementById("ctrlModeText").textContent=["Automatic","Manual","Holiday"][d.mode||0];
var errs=d.errors||[];
var ht=document.getElementById("healthTitle"),hs=document.getElementById("healthSub");
if(errs.length===0){ht.className="health-title ok";ht.innerHTML="System Healthy";hs.textContent="All systems normal";}
else{ht.className="health-title err";ht.innerHTML=errs.length+" Alert(s)";hs.textContent=errs.join(" - ");}
var al=document.getElementById("alertList");
if(errs.length>0){var em2={"ERR_01":"Sensor","ERR_02":"Dry Run","ERR_03":"Leak","ERR_04":"Max Run","DRY_LOCK":"Locked","KILL_SW":"Kill","LEAK":"Leak"};
var h="";for(var i=0;i<errs.length;i++)h+="<div class=\"alert-card\"><div class=\"alert-code\">"+errs[i]+"</div><div class=\"alert-msg\">"+(em2[errs[i]]||errs[i])+"</div></div>";
al.innerHTML=h;}else al.innerHTML="<div style=\"font-size:11px;color:var(--text2);padding:8px\">No alerts</div>";}

function updWifiStatus(){var box=document.getElementById("wifiStatusBox");
var txt=document.getElementById("wifiStatusText");
var info=document.getElementById("wifiInfo");
if(!box||!txt)return;
var st=data.wifiState||0;
var msg=data.wifiStatusMsg||"Idle";
var icon="";
if(st===2){box.className="wifi-status-box connected";icon="OK ";}
else if(st===1){box.className="wifi-status-box connecting";icon="<span class=\"spinner\"></span>";}
else if(st===3){box.className="wifi-status-box failed";icon="X ";}
else{box.className="wifi-status-box idle";icon="O ";}
txt.innerHTML=icon+msg;
if(info){info.innerHTML="<strong>AP IP:</strong> 192.168.4.1 | <strong>SSID:</strong> SmartTank_AP<br>"
+(data.wifiConnected?"<strong>STA IP:</strong> "+data.staIP+"<br><strong>Network:</strong> "+data.wifiSSID:"<strong>STA:</strong> Not connected")
+(data.savedSSID?"<br><strong>Saved SSID:</strong> "+data.savedSSID+" (pass len: "+data.savedPassLen+")":"");}}

function updAdmin(){var ab=document.getElementById("adminBtn"),at=document.getElementById("adminText");
if(isAdmin){ab.className="admin-btn unlocked";at.textContent="Admin";}
else{ab.className="admin-btn";at.textContent="Locked";}}

function switchTab(t){var tabs=document.querySelectorAll(".tab");
for(var i=0;i<tabs.length;i++)tabs[i].classList.remove("active");
var tcs=document.querySelectorAll(".tab-content");
for(var i=0;i<tcs.length;i++)tcs[i].classList.remove("active");
document.querySelector(".tab[data-tab=\""+t+"\"]").classList.add("active");
document.getElementById("tab-"+t).classList.add("active");
if(t==="dashboard")loadHist();
if(t==="config"&&isAdmin)loadCfg();
if(t==="wifi")updWifiStatus();}

function navTo(p){switchTab(p);}
function toggleAdmin(){if(isAdmin){isAdmin=false;adminPw="";updAdmin();showT("Logged out","ok");}
else{showM("loginModal");setTimeout(function(){document.getElementById("loginPw").focus();},300);}}
function doLogin(){var pw=document.getElementById("loginPw").value;if(!pw){showT("Enter password","err");return;}
wsSend({action:"login",password:pw});}
function cycleMode(){if(!isAdmin){showT("Admin required","err");return;}
wsSend({action:"setMode",mode:((data.mode||0)+1)%3});}
function toggleMotor(n){if(!isAdmin){showT("Admin required","err");return;}
var cur=n===1?data.motor1:data.motor2;wsSend({action:"motor"+n,state:!cur});}
function eStop(){wsSend({action:"emergencyStop"});}
function clearAll(){if(!isAdmin){showT("Admin required","err");return;}wsSend({action:"clearErrors"});}
function doReboot(){if(!isAdmin){showT("Admin required","err");return;}if(confirm("Reboot?"))wsSend({action:"reboot"});}
function doFactoryReset(){var pw=document.getElementById("resetPw").value;if(!pw){showT("Enter pw","err");return;}
if(!confirm("Sure?"))return;ws.send(JSON.stringify({action:"factoryReset",password:pw}));closeM("resetModal");}
function doChangePw(){var o=document.getElementById("oldPw").value,n=document.getElementById("newPw").value,
n2=document.getElementById("newPw2").value;if(o!==adminPw){showT("Wrong pw","err");return;}
if(n.length<4){showT("Min 4 chars","err");return;}if(n!==n2){showT("Mismatch","err");return;}
wsSend({action:"changePassword",newPassword:n});adminPw=n;closeM("pwModal");}

function togglePassVisibility(){var i=document.getElementById("wf_pass");var c=document.getElementById("showPassChk");
i.type=c.checked?"text":"password";}

function updatePassPreview(){var p=document.getElementById("wf_pass").value;
var prev=document.getElementById("passPreview");
var len=document.getElementById("passLen");
var fl=document.getElementById("passFL");
if(p.length===0){prev.textContent="(empty)";len.textContent="0";fl.textContent="--";return;}
prev.textContent=p;
len.textContent=p.length;
fl.textContent="["+p.charAt(0)+"]...["+p.charAt(p.length-1)+"]";}

document.addEventListener("DOMContentLoaded",function(){
  var pi=document.getElementById("wf_pass");
  if(pi){pi.addEventListener("input",updatePassPreview);pi.addEventListener("change",updatePassPreview);}
});

function submitWifiForm(ev){ev.preventDefault();
if(!isAdmin){showT("Admin required","err");return false;}
var ssid=document.getElementById("wf_ssid").value.trim();
var pass=document.getElementById("wf_pass").value;
if(!ssid){showT("Enter SSID","err");return false;}
showT("Submitting via HTTP form...","ok");

var fd=new FormData();
fd.append("ssid",ssid);
fd.append("pass",pass);
fd.append("admin",adminPw);

fetch("/wifi-connect",{method:"POST",body:fd})
.then(function(r){return r.text();})
.then(function(t){console.log("Server response:",t);showT("Sent! Wait 25 sec for status","ok");})
.catch(function(e){showT("Submit failed: "+e,"err");});

return false;}

function scanWifi(){if(!isAdmin){showT("Admin required","err");return;}
showT("Scanning...","ok");
document.getElementById("wifiList").innerHTML="<div style=\"font-size:11px;color:var(--accent);padding:6px\"><span class=\"spinner\"></span>Scanning...</div>";
wsSend({action:"scanWifi"});}

function renderWifiList(networks){var list=document.getElementById("wifiList");
if(!networks||networks.length===0){list.innerHTML="<div style=\"font-size:11px;color:var(--text2);padding:6px\">No networks found</div>";return;}
var h="";for(var i=0;i<networks.length;i++){var n=networks[i];
var bars=n.rssi>-50?"||||":n.rssi>-60?"||| ":n.rssi>-70?"||  ":"|   ";
var lock=n.secure?" *":"";
h+="<div class=\"wifi-item\" onclick=\"selectWifi('"+n.ssid.replace(/'/g,"\\'")+"')\">"
+"<div><div class=\"wifi-name\">"+n.ssid+lock+"</div><div style=\"font-size:9px;color:var(--text2)\">Ch "+(n.ch||"?")+"</div></div>"
+"<div class=\"wifi-rssi\">"+bars+" "+n.rssi+"dBm</div></div>";}
list.innerHTML=h;}

function selectWifi(ssid){document.getElementById("wf_ssid").value=ssid;
document.getElementById("wf_pass").value="";
updatePassPreview();
showT("SSID set: "+ssid+". Now enter password","ok");
document.getElementById("wf_pass").focus();}

function forgetWifi(){if(!isAdmin){showT("Admin required","err");return;}
if(!confirm("Forget?"))return;wsSend({action:"forgetWifi"});}

function loadCfg(){wsSend({action:"getConfig"});}
function fillCfg(c){document.getElementById("c_th").value=c.tankHeightCm||200;
document.getElementById("c_mv").value=c.tankMaxVolumeLiters||1000;
document.getElementById("c_son").value=c.servoOnLevel!=null?c.servoOnLevel:95;
document.getElementById("c_soff").value=c.servoOffLevel!=null?c.servoOffLevel:50;
document.getElementById("c_dr").value=c.dryRunTimeoutMinutes!=null?c.dryRunTimeoutMinutes:3;
document.getElementById("c_mr").value=c.maxSingleRunTimeoutMinutes!=null?c.maxSingleRunTimeoutMinutes:10;
document.getElementById("c_ss").value=c.silentStartHour!=null?c.silentStartHour:0;
document.getElementById("c_se").value=c.silentEndHour!=null?c.silentEndHour:6;
document.getElementById("c_w").value=c.pumpWattage||746;
document.getElementById("c_uc").value=c.unitCost||9;
document.getElementById("c_bt").value=c.blynkAuth||"";
if(c.schedules&&c.schedules.length>0){schedules=[];
for(var i=0;i<c.schedules.length;i++)schedules.push(c.schedules[i]);
while(schedules.length<10)schedules.push({sh:0,sm:0,eh:0,em:0,en:false});}renderSchedules();}

function renderSchedules(){var c=document.getElementById("schedContainer"),h="";
for(var i=0;i<10;i++){var s=schedules[i];
h+="<div class=\"sched-card\"><div style=\"display:flex;justify-content:space-between;margin-bottom:4px\"><div style=\"font-size:11px;color:var(--text)\">S"+(i+1)+"</div>"
+"<label style=\"font-size:10px\"><input type=\"checkbox\" "+(s.en?"checked":"")+" onchange=\"schedToggle("+i+",this.checked)\"> On</label></div>"
+"<div class=\"sched-row\">S:<input type=\"number\" min=\"0\" max=\"23\" value=\""+s.sh+"\" onchange=\"schedSet("+i+",'sh',this.value)\">:"
+"<input type=\"number\" min=\"0\" max=\"59\" value=\""+s.sm+"\" onchange=\"schedSet("+i+",'sm',this.value)\">"
+" E:<input type=\"number\" min=\"0\" max=\"23\" value=\""+s.eh+"\" onchange=\"schedSet("+i+",'eh',this.value)\">:"
+"<input type=\"number\" min=\"0\" max=\"59\" value=\""+s.em+"\" onchange=\"schedSet("+i+",'em',this.value)\"></div></div>";}c.innerHTML=h;}

function schedToggle(i,v){schedules[i].en=v;}
function schedSet(i,k,v){v=parseInt(v)||0;if(k==="sh"||k==="eh")v=Math.max(0,Math.min(23,v));
else v=Math.max(0,Math.min(59,v));schedules[i][k]=v;}

function saveCfg(){if(!isAdmin){showT("Admin required","err");return;}
var c={tankHeightCm:parseFloat(document.getElementById("c_th").value)||200,
tankMaxVolumeLiters:parseFloat(document.getElementById("c_mv").value)||1000,
servoOnLevel:parseInt(document.getElementById("c_son").value)||95,
servoOffLevel:parseInt(document.getElementById("c_soff").value)||50,
dryRunTimeoutMinutes:parseInt(document.getElementById("c_dr").value)||3,
maxSingleRunTimeoutMinutes:parseInt(document.getElementById("c_mr").value)||10,
silentStartHour:parseInt(document.getElementById("c_ss").value)||0,
silentEndHour:parseInt(document.getElementById("c_se").value)||6,
pumpWattage:parseFloat(document.getElementById("c_w").value)||746,
unitCost:parseFloat(document.getElementById("c_uc").value)||9,
blynkAuth:document.getElementById("c_bt").value||"",schedules:schedules};
wsSend({action:"saveConfig",config:c});}

function loadHist(){histPage=0;wsSend({action:"getHistory",page:0});}
function loadMoreHist(){histPage++;wsSend({action:"getHistory",page:histPage});}
function renderHist(d){var hl=document.getElementById("historyList");
if(histPage===0)hl.innerHTML="";var ev=d.events||[];
if(ev.length===0&&histPage===0){hl.innerHTML="<div style=\"font-size:10px;color:var(--text2);padding:6px\">No events</div>";
document.getElementById("moreBtn").style.display="none";return;}
for(var i=0;i<ev.length;i++){hl.innerHTML+="<div class=\"log-entry\"><div class=\"log-time\">"+ev[i].time
+"</div><div class=\"log-event\">"+ev[i].event+"</div><div><span class=\"log-source\">"+ev[i].source
+"</span> - <span class=\"log-level\">"+ev[i].level.toFixed(1)+"%</span></div></div>";}
document.getElementById("moreBtn").style.display=ev.length<10?"none":"block";}

function showM(id){document.getElementById(id).classList.add("show");}
function closeM(id){document.getElementById(id).classList.remove("show");}
function showT(m,t){var x=document.getElementById("toast");x.textContent=m;
x.className="toast show"+(t==="err"?" error":"");setTimeout(function(){x.className="toast";},3500);}

loadTheme();renderSchedules();connectWS();setTimeout(loadHist,2000);
</script></body></html>
)KOIPAGE";

void sendIndexPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "max-age=3600");
  server.send(200, "text/html", "");
  server.sendContent_P(HTML_P1); yield();
  server.sendContent_P(HTML_P2); yield();
  server.sendContent_P(HTML_P3); yield();
  server.sendContent("");
  server.client().stop();
}

uint32_t calcChecksum(SystemConfig* c) {
  uint32_t s = 0; uint8_t* p = (uint8_t*)c;
  for (size_t i = 0; i < sizeof(SystemConfig) - sizeof(uint32_t); i++) s += p[i] * (i + 1);
  return s;
}

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = EEPROM_MAGIC;
  cfg.tankHeightCm = 200.0f;
  cfg.tankMaxVolumeLiters = 1000.0f;
  cfg.dryRunTimeoutMinutes = 3;
  cfg.maxSingleRunTimeoutMinutes = 10;
  cfg.servoOnLevel = 95;
  cfg.servoOffLevel = 50;
  cfg.motor2Schedules[0] = {6, 0, 8, 0, true};
  cfg.systemOperatingMode = 0;
  cfg.pumpWattage = 746.0f;
  cfg.unitCost = 9.0f;
  strncpy(cfg.adminPassword, "jamalmd1", 31);
  cfg.silentStartHour = 0;
  cfg.silentEndHour = 6;
  cfg.checksum = calcChecksum(&cfg);
}

void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC || cfg.checksum != calcChecksum(&cfg)) {
    setDefaults(); saveConfig();
  }
}

void saveConfig() {
  cfg.magic = EEPROM_MAGIC;
  cfg.checksum = calcChecksum(&cfg);
  SystemConfig ex; EEPROM.get(0, ex);
  if (memcmp(&ex, &cfg, sizeof(SystemConfig)) != 0) {
    EEPROM.put(0, cfg); EEPROM.commit();
  }
}

void setMotor1(bool on, const char* src) {
  if (on && !motor1Running) {
    digitalWrite(RELAY_MOTOR1, RELAY_ON); motor1Running = true;
    motor1StartTime = millis(); motor1StartLevel = tankLevelPercent;
    wakeOLED(); addEventLog("Servo ON", src, tankLevelPercent);
  } else if (!on && motor1Running) {
    digitalWrite(RELAY_MOTOR1, RELAY_OFF); motor1Running = false;
    pulseMode = false; wakeOLED(); addEventLog("Servo OFF", src, tankLevelPercent);
  }
}

void setMotor2(bool on, const char* src) {
  if (on && !motor2Running) {
    digitalWrite(RELAY_MOTOR2, RELAY_ON); motor2Running = true;
    motor2StartTime = millis(); motor2StartLevel = tankLevelPercent;
    motor2LastRunEpoch = millis() / 1000;
    wakeOLED(); addEventLog("Motor ON", src, tankLevelPercent);
  } else if (!on && motor2Running) {
    digitalWrite(RELAY_MOTOR2, RELAY_OFF); motor2Running = false;
    wakeOLED(); addEventLog("Motor OFF", src, tankLevelPercent);
  }
}

bool isInMotor2Window() {
  int h = timeClient.getHours(), m = timeClient.getMinutes();
  int nm = h * 60 + m;
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!cfg.motor2Schedules[i].enabled) continue;
    int sm = cfg.motor2Schedules[i].startHour * 60 + cfg.motor2Schedules[i].startMinute;
    int em = cfg.motor2Schedules[i].endHour * 60 + cfg.motor2Schedules[i].endMinute;
    if (sm == em) continue;
    if (sm <= em) { if (nm >= sm && nm < em) return true; }
    else { if (nm >= sm || nm < em) return true; }
  }
  return false;
}

bool isInSilentHours() {
  int h = timeClient.getHours();
  if (cfg.silentStartHour <= cfg.silentEndHour) return (h >= cfg.silentStartHour && h < cfg.silentEndHour);
  return (h >= cfg.silentStartHour || h < cfg.silentEndHour);
}

void addEventLog(const char* et, const char* src, float lev) {
  int idx = eventHead;
  strncpy(eventHistory[idx].timestamp, getFormattedTime().c_str(), 39);
  strncpy(eventHistory[idx].eventType, et, 39);
  strncpy(eventHistory[idx].triggerSource, src, 27);
  eventHistory[idx].tankLevel = lev; eventHistory[idx].valid = true;
  eventHead = (eventHead + 1) % MAX_HISTORY_ENTRIES;
  if (eventCount < MAX_HISTORY_ENTRIES) eventCount++;
  Serial.printf("[LOG] %s | %s | %.1f%%\n", et, src, lev);
}

String getFormattedTime() {
  time_t rt = timeClient.getEpochTime(); struct tm* ti = localtime(&rt);
  const char* dn[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  char buf[42]; snprintf(buf, 42, "%02d/%02d/%04d|%s|%02d:%02d:%02d",
    ti->tm_mday, ti->tm_mon+1, ti->tm_year+1900, dn[ti->tm_wday],
    ti->tm_hour, ti->tm_min, ti->tm_sec); return String(buf);
}

String getShortTime() {
  time_t rt = timeClient.getEpochTime(); struct tm* ti = localtime(&rt);
  char buf[10]; int h = ti->tm_hour; const char* ap = h >= 12 ? "PM" : "AM";
  if (h > 12) h -= 12; if (h == 0) h = 12;
  snprintf(buf, 10, "%02d:%02d%s", h, ti->tm_min, ap); return String(buf);
}

void wakeOLED() { if (oledInitialized && oledSleeping) { display.ssd1306_command(SSD1306_DISPLAYON); oledSleeping = false; } lastOLEDActivity = millis(); }
void sleepOLED() { if (oledInitialized && !oledSleeping) { display.ssd1306_command(SSD1306_DISPLAYOFF); oledSleeping = true; } }

void updateOLED() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  switch (oledPage) {
    case 0:
      display.setCursor(0, 0); display.print(F("KOI ")); 
      display.print(WiFi.status() == WL_CONNECTED ? F("[NET+AP]") : F("[AP only]"));
      display.setCursor(0, 12); display.setTextSize(2);
      display.printf("%.0f%%", tankLevelPercent); display.setTextSize(1);
      display.printf(" %.0fL", tankVolumeLiters);
      display.setCursor(0, 32); display.printf("Sv:%s Mt:%s", motor1Running?"ON":"OF", motor2Running?"ON":"OF");
      display.setCursor(0, 44); display.printf("Mode:%s", cfg.systemOperatingMode==0?"AUTO":cfg.systemOperatingMode==1?"MAN":"HOL");
      display.setCursor(0, 56);
      if (dryRunLocked || leakageAlert) display.print(F("! ALERT"));
      else if (killSwitchActive) display.print(F("! KILL"));
      else display.print(getShortTime());
      break;
    case 1:
      display.setCursor(0, 0); display.println(F("== ACCESS APP =="));
      display.println(F("WiFi:SmartTank_AP"));
      display.println(F("Pass:12345678"));
      display.println(F("URL:192.168.4.1"));
      display.println(F("Login:jamalmd1"));
      if (WiFi.status() == WL_CONNECTED) {
        display.print(F("Net:")); display.println(WiFi.localIP());
      }
      break;
    case 2: {
      display.setCursor(0, 0); display.println(F("== STATUS =="));
      display.print(F("WiFi:"));
      display.println(wifiState == WIFI_STATE_CONNECTING ? F("Connect") :
                      wifiState == WIFI_STATE_CONNECTED ? F("OK") :
                      wifiState == WIFI_STATE_FAILED ? F("Failed") : F("Idle"));
      if (lastDisconnectReason > 0) display.printf("Err:%d\n", lastDisconnectReason);
      display.printf("PassLen:%d\n", strlen(cfg.wifiPass));
      display.printf("Heap:%d", ESP.getFreeHeap());
      break; }
  }
  display.display();
}

void setupBlynk() { blynkEnabled = strlen(cfg.blynkAuth) > 5; if (blynkEnabled) Blynk.config(cfg.blynkAuth); }

void runBlynk() {
  if (!blynkEnabled || WiFi.status() != WL_CONNECTED) return;
  if (!Blynk.connected()) Blynk.connect(2);
  else { Blynk.run(); static unsigned long ls = 0;
    if (millis() - ls >= 3000) { ls = millis();
      Blynk.virtualWrite(V0, tankLevelPercent); Blynk.virtualWrite(V1, tankVolumeLiters);
      Blynk.virtualWrite(V2, motor1Running?1:0); Blynk.virtualWrite(V3, motor2Running?1:0);
      Blynk.virtualWrite(V4, cfg.systemOperatingMode); } }
}

BLYNK_WRITE(V10) { motor1ManualOverride = true; setMotor1(param.asInt(), "Blynk"); }
BLYNK_WRITE(V11) { motor2ManualOverride = true; setMotor2(param.asInt(), "Blynk"); }
BLYNK_WRITE(V12) { cfg.systemOperatingMode = param.asInt(); cfgDirty = true; }

void clearErrors() { dryRunLocked = false; leakageAlert = false;
  for (int i = 0; i < 5; i++) errorCodes[i] = "";
  addEventLog("Errors Cleared", "User", tankLevelPercent); wakeOLED(); }

String buildStatusJSON() {
  StaticJsonDocument<1024> d;
  d["level"]=round(tankLevelPercent*10)/10.0; d["volume"]=round(tankVolumeLiters);
  d["maxVolume"]=cfg.tankMaxVolumeLiters; d["distance"]=round(filteredDistance*10)/10.0;
  d["temp"]=ambientTemp; d["soundSpeed"]=round((331.3f+0.606f*ambientTemp)*10)/10.0;
  d["motor1"]=motor1Running; d["motor2"]=motor2Running; d["mode"]=cfg.systemOperatingMode;
  d["rssi"]=WiFi.RSSI(); float kwh=0; for(int i=0;i<24;i++)kwh+=hourlyKwh[i];
  d["dailyKwh"]=round(kwh*100)/100.0; d["dailyCost"]=round(kwh*cfg.unitCost*100)/100.0;
  d["killSwitch"]=killSwitchActive; d["dryRunLocked"]=dryRunLocked;
  d["leakageAlert"]=leakageAlert; d["pulseMode"]=pulseMode;
  d["m1RunMin"]=motor1CumulativeRunSec/60; d["m2RunMin"]=motor2CumulativeRunSec/60;
  d["uptime"]=(millis()-bootTime)/1000; d["time"]=getShortTime();
  d["wifiConnected"]=(WiFi.status()==WL_CONNECTED);
  d["wifiSSID"]=WiFi.SSID();
  d["staIP"]=WiFi.localIP().toString();
  d["wifiState"]=wifiState;
  d["wifiStatusMsg"]=wifiStatusMsg;
  d["savedSSID"]=cfg.wifiSSID;
  d["savedPassLen"]=(int)strlen(cfg.wifiPass);
  JsonArray errs=d.createNestedArray("errors");
  for(int i=0;i<5;i++) if(errorCodes[i].length()>0)errs.add(errorCodes[i]);
  if(dryRunLocked)errs.add("DRY_LOCK"); if(killSwitchActive)errs.add("KILL_SW"); if(leakageAlert)errs.add("LEAK");
  String out; serializeJson(d, out); return out;
}

String buildHistoryJSON(int page) {
  StaticJsonDocument<2048> d; JsonArray arr=d.createNestedArray("events");
  d["total"]=eventCount; d["page"]=page;
  int skip=page*HISTORY_PAGE_SIZE, shown=0;
  for(int i=0;i<eventCount&&shown<HISTORY_PAGE_SIZE;i++){
    int idx=(eventHead-1-i+MAX_HISTORY_ENTRIES)%MAX_HISTORY_ENTRIES;
    if(!eventHistory[idx].valid)continue; if(i<skip)continue;
    JsonObject o=arr.createNestedObject(); o["time"]=eventHistory[idx].timestamp;
    o["event"]=eventHistory[idx].eventType; o["source"]=eventHistory[idx].triggerSource;
    o["level"]=round(eventHistory[idx].tankLevel*10)/10.0; shown++;}
  String out; serializeJson(d, out); return out;
}

String buildConfigJSON() {
  StaticJsonDocument<1024> d;
  d["tankHeightCm"]=cfg.tankHeightCm; d["tankMaxVolumeLiters"]=cfg.tankMaxVolumeLiters;
  d["dryRunTimeoutMinutes"]=cfg.dryRunTimeoutMinutes; d["maxSingleRunTimeoutMinutes"]=cfg.maxSingleRunTimeoutMinutes;
  d["servoOnLevel"]=cfg.servoOnLevel; d["servoOffLevel"]=cfg.servoOffLevel;
  d["systemOperatingMode"]=cfg.systemOperatingMode; d["pumpWattage"]=cfg.pumpWattage;
  d["unitCost"]=cfg.unitCost; d["silentStartHour"]=cfg.silentStartHour;
  d["silentEndHour"]=cfg.silentEndHour; d["blynkAuth"]=cfg.blynkAuth;
  JsonArray sa=d.createNestedArray("schedules");
  for(int i=0;i<MAX_SCHEDULES;i++){JsonObject s=sa.createNestedObject();
    s["sh"]=cfg.motor2Schedules[i].startHour; s["sm"]=cfg.motor2Schedules[i].startMinute;
    s["eh"]=cfg.motor2Schedules[i].endHour; s["em"]=cfg.motor2Schedules[i].endMinute;
    s["en"]=cfg.motor2Schedules[i].enabled;}
  String out; serializeJson(d, out); return out;
}

String buildPowerJSON() {
  StaticJsonDocument<512> d; JsonArray hr=d.createNestedArray("hourly");
  for(int i=0;i<24;i++)hr.add(round(hourlyKwh[i]*1000)/1000.0);
  float kwh=0; for(int i=0;i<24;i++)kwh+=hourlyKwh[i];
  d["totalKwh"]=round(kwh*100)/100.0; d["totalCost"]=round(kwh*cfg.unitCost*100)/100.0;
  String out; serializeJson(d, out); return out;
}

void applyConfigFromJSON(JsonDocument& d) {
  if(d.containsKey("tankHeightCm"))cfg.tankHeightCm=d["tankHeightCm"];
  if(d.containsKey("tankMaxVolumeLiters"))cfg.tankMaxVolumeLiters=d["tankMaxVolumeLiters"];
  if(d.containsKey("dryRunTimeoutMinutes"))cfg.dryRunTimeoutMinutes=constrain((int)d["dryRunTimeoutMinutes"],0,59);
  if(d.containsKey("maxSingleRunTimeoutMinutes"))cfg.maxSingleRunTimeoutMinutes=constrain((int)d["maxSingleRunTimeoutMinutes"],0,599);
  if(d.containsKey("servoOnLevel"))cfg.servoOnLevel=constrain((int)d["servoOnLevel"],0,100);
  if(d.containsKey("servoOffLevel"))cfg.servoOffLevel=constrain((int)d["servoOffLevel"],0,100);
  if(d.containsKey("systemOperatingMode"))cfg.systemOperatingMode=constrain((int)d["systemOperatingMode"],0,2);
  if(d.containsKey("pumpWattage"))cfg.pumpWattage=d["pumpWattage"];
  if(d.containsKey("unitCost"))cfg.unitCost=d["unitCost"];
  if(d.containsKey("silentStartHour"))cfg.silentStartHour=constrain((int)d["silentStartHour"],0,23);
  if(d.containsKey("silentEndHour"))cfg.silentEndHour=constrain((int)d["silentEndHour"],0,23);
  if(d.containsKey("adminPassword")){String pw=d["adminPassword"].as<String>();
    if(pw.length()>=4&&pw.length()<31)strncpy(cfg.adminPassword,pw.c_str(),31);}
  if(d.containsKey("blynkAuth")){strncpy(cfg.blynkAuth,d["blynkAuth"].as<const char*>(),63);setupBlynk();}
  if(d.containsKey("schedules")){JsonArray arr=d["schedules"];int i=0;
    for(JsonObject s:arr){if(i>=MAX_SCHEDULES)break;
      cfg.motor2Schedules[i].startHour=constrain((int)(s["sh"]|0),0,23);
      cfg.motor2Schedules[i].startMinute=constrain((int)(s["sm"]|0),0,59);
      cfg.motor2Schedules[i].endHour=constrain((int)(s["eh"]|0),0,23);
      cfg.motor2Schedules[i].endMinute=constrain((int)(s["em"]|0),0,59);
      cfg.motor2Schedules[i].enabled=s["en"]|false;i++;}}
  cfgDirty=true; addEventLog("Config Updated","Web",tankLevelPercent);
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if(type==WStype_CONNECTED){String m=buildStatusJSON();webSocket.sendTXT(num,m);}
  else if(type==WStype_TEXT)handleWSMessage(num,payload,length);
}

void handleWSMessage(uint8_t num, uint8_t* data, size_t len) {
  StaticJsonDocument<1024> doc;
  if(deserializeJson(doc,data,len)){String e="{\"error\":\"bad json\"}";webSocket.sendTXT(num,e);return;}
  String action=doc["action"]|"";
  if(action=="login"){String pw=doc["password"]|"";String r;
    if(pw==String(cfg.adminPassword))r="{\"auth\":\"ok\"}";else r="{\"auth\":\"fail\"}";
    webSocket.sendTXT(num,r);return;}
  String pw=doc["password"]|"";bool ia=(pw==String(cfg.adminPassword));
  String ae="{\"error\":\"auth\"}",ok="{\"status\":\"ok\"}";

  if(action=="motor1"){if(!ia){webSocket.sendTXT(num,ae);return;}motor1ManualOverride=true;
    setMotor1(doc["state"]|false,"Web");if(!(doc["state"]|false))motor1ManualOverride=false;webSocket.sendTXT(num,ok);}
  else if(action=="motor2"){if(!ia){webSocket.sendTXT(num,ae);return;}motor2ManualOverride=true;
    setMotor2(doc["state"]|false,"Web");if(!(doc["state"]|false))motor2ManualOverride=false;webSocket.sendTXT(num,ok);}
  else if(action=="setMode"){if(!ia){webSocket.sendTXT(num,ae);return;}
    cfg.systemOperatingMode=constrain((int)(doc["mode"]|0),0,2);cfgDirty=true;
    motor1ManualOverride=false;motor2ManualOverride=false;webSocket.sendTXT(num,ok);}
  else if(action=="clearErrors"){if(!ia){webSocket.sendTXT(num,ae);return;}clearErrors();webSocket.sendTXT(num,ok);}
  else if(action=="emergencyStop"){if(motor1Running)setMotor1(false,"E-Stop");if(motor2Running)setMotor2(false,"E-Stop");
    motor1ManualOverride=false;motor2ManualOverride=false;webSocket.sendTXT(num,ok);}
  else if(action=="saveConfig"){if(!ia){webSocket.sendTXT(num,ae);return;}JsonObject c=doc["config"];
    if(c.isNull()){String e="{\"error\":\"no cfg\"}";webSocket.sendTXT(num,e);return;}
    StaticJsonDocument<1024> cd;for(JsonPair kv:c)cd[kv.key()]=kv.value();applyConfigFromJSON(cd);
    String r="{\"status\":\"ok\",\"msg\":\"Config saved\"}";webSocket.sendTXT(num,r);}
  else if(action=="getConfig"){String c=buildConfigJSON();webSocket.sendTXT(num,c);}
  else if(action=="getHistory"){int p=doc["page"]|0;String h=buildHistoryJSON(p);webSocket.sendTXT(num,h);}
  else if(action=="scanWifi"){if(!ia){webSocket.sendTXT(num,ae);return;}
    scanWiFiNetworks();webSocket.sendTXT(num,wifiScanResult);}
  else if(action=="forgetWifi"){if(!ia){webSocket.sendTXT(num,ae);return;}
    memset(cfg.wifiSSID,0,33);memset(cfg.wifiPass,0,65);saveConfig();
    WiFi.disconnect(true);
    wifiState = WIFI_STATE_IDLE;
    wifiStatusMsg = "WiFi forgotten";
    String r="{\"status\":\"ok\",\"msg\":\"WiFi forgotten\"}";webSocket.sendTXT(num,r);}
  else if(action=="reboot"){if(!ia){webSocket.sendTXT(num,ae);return;}
    String r="{\"status\":\"rebooting\"}";webSocket.sendTXT(num,r);saveConfig();delay(500);ESP.restart();}
  else if(action=="changePassword"){if(!ia){webSocket.sendTXT(num,ae);return;}
    String np=doc["newPassword"]|"";if(np.length()>=4&&np.length()<31){
      strncpy(cfg.adminPassword,np.c_str(),31);cfgDirty=true;
      String r="{\"status\":\"ok\",\"msg\":\"Password changed\"}";webSocket.sendTXT(num,r);}
    else{String e="{\"error\":\"Min 4 chars\"}";webSocket.sendTXT(num,e);}}
  else if(action=="factoryReset"){if(!ia){webSocket.sendTXT(num,ae);return;}
    String r="{\"status\":\"ok\",\"msg\":\"Reset!\"}";webSocket.sendTXT(num,r);
    for(int i=0;i<EEPROM_SIZE;i++)EEPROM.write(i,0xFF);EEPROM.commit();
    WiFi.disconnect(true);delay(500);ESP.eraseConfig();delay(500);ESP.restart();}
}

void broadcastWSData(){if(webSocket.connectedClients()>0){String j=buildStatusJSON();webSocket.broadcastTXT(j);}}

// CRITICAL: HTTP form handler for WiFi - bypasses all JSON encoding issues!
void handleWifiConnect() {
  if (!server.hasArg("ssid") || !server.hasArg("admin")) {
    server.send(400, "text/plain", "Missing parameters");
    return;
  }
  
  String adminPw = server.arg("admin");
  if (adminPw != String(cfg.adminPassword)) {
    server.send(403, "text/plain", "Wrong admin password");
    return;
  }
  
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  
  // Trim whitespace
  ssid.trim();
  // Don't trim password - whitespace might be part of it!
  
  Serial.println(F("\n=========================="));
  Serial.println(F("[HTTP-FORM] WiFi Connect Request"));
  Serial.printf("[HTTP-FORM] SSID: '%s' (length: %d)\n", ssid.c_str(), ssid.length());
  Serial.printf("[HTTP-FORM] Pass: '%s' (length: %d)\n", pass.c_str(), pass.length());
  Serial.println(F("[HTTP-FORM] Pass char by char:"));
  for (size_t i = 0; i < pass.length(); i++) {
    Serial.printf("  [%d] '%c' (ASCII %d / 0x%02X)\n", i, pass[i], (int)pass[i], (int)pass[i]);
  }
  Serial.println(F("=========================="));
  
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400, "text/plain", "Invalid SSID length");
    return;
  }
  if (pass.length() > 64) {
    server.send(400, "text/plain", "Password too long");
    return;
  }
  
  // Save to config
  memset(cfg.wifiSSID, 0, 33);
  memset(cfg.wifiPass, 0, 65);
  strncpy(cfg.wifiSSID, ssid.c_str(), 32);
  strncpy(cfg.wifiPass, pass.c_str(), 64);
  saveConfig();
  
  addEventLog("WiFi Setup", ssid.c_str(), tankLevelPercent);
  
  // Start non-blocking connect
  startWiFiConnect();
  
  String resp = "OK. Saved SSID '" + ssid + "' with password length " + String(pass.length()) + ". Connecting now...";
  server.send(200, "text/plain", resp);
}

void setupWebServer() {
  server.on("/", HTTP_GET, sendIndexPage);
  server.on("/api/status", HTTP_GET, [](){ server.send(200, "application/json", buildStatusJSON()); });
  server.on("/api/config", HTTP_GET, [](){ server.send(200, "application/json", buildConfigJSON()); });
  server.on("/api/power", HTTP_GET, [](){ server.send(200, "application/json", buildPowerJSON()); });
  server.on("/api/history", HTTP_GET, [](){ int p=0; if(server.hasArg("page"))p=server.arg("page").toInt();
    server.send(200, "application/json", buildHistoryJSON(p)); });
  
  // CRITICAL: HTTP form for WiFi (no JSON escape issues!)
  server.on("/wifi-connect", HTTP_POST, handleWifiConnect);
  
  server.on("/generate_204", HTTP_GET, sendIndexPage);
  server.on("/fwlink", HTTP_GET, sendIndexPage);
  server.on("/hotspot-detect.html", HTTP_GET, sendIndexPage);
  server.onNotFound([](){ sendIndexPage(); });
  server.begin();
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println(F("\n\n=========================================="));
  Serial.println(F("  KOI Smart Tank v4.5 - Pass Fixed"));
  Serial.println(F("=========================================="));

  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(RELAY_MOTOR1, OUTPUT); pinMode(RELAY_MOTOR2, OUTPUT);
  pinMode(PIN_KILL_SW, INPUT_PULLUP);
  digitalWrite(RELAY_MOTOR1, RELAY_OFF); digitalWrite(RELAY_MOTOR2, RELAY_OFF);

  EEPROM.begin(EEPROM_SIZE); loadConfig();

  Wire.begin(PIN_SDA, PIN_SCL);
  if(display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    oledInitialized=true; display.clearDisplay(); display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE); display.setCursor(0,0);
    display.println(F("KOI v4.5 Booting"));
    display.println(F(""));
    display.println(F("WiFi: SmartTank_AP"));
    display.println(F("Pass: 12345678"));
    display.println(F("URL:  192.168.4.1"));
    display.println(F("Login: jamalmd1"));
    display.display();
  }
  lastOLEDActivity=millis();

  WiFi.persistent(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);
  WiFi.setAutoReconnect(false);
  
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  
  dnsServer.start(53, "*", apIP);
  
  Serial.println(F("\n[AP] Started!"));
  Serial.print(F("[AP] SSID: ")); Serial.println(AP_SSID);
  Serial.print(F("[AP] IP: ")); Serial.println(WiFi.softAPIP());
  Serial.print(F("[STA] MAC: ")); Serial.println(WiFi.macAddress());

  wifiConnectHandler = WiFi.onStationModeConnected(onWiFiConnected);
  wifiGotIPHandler = WiFi.onStationModeGotIP(onWiFiGotIP);
  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWiFiDisconnected);

  if (strlen(cfg.wifiSSID) > 0) {
    Serial.printf("[WIFI] Saved SSID: %s, pass len: %d\n", cfg.wifiSSID, strlen(cfg.wifiPass));
    delay(1000);
    startWiFiConnect();
  }

  timeClient.begin();
  bootTime=millis();

  ArduinoOTA.setHostname("KOI-SmartTank"); ArduinoOTA.begin();
  setupWebServer(); webSocket.begin(); webSocket.onEvent(onWebSocketEvent);
  setupBlynk();

  memset(sonicBuffer,0,sizeof(sonicBuffer)); memset(hourlyKwh,0,sizeof(hourlyKwh));
  for(int i=0;i<MAX_HISTORY_ENTRIES;i++)eventHistory[i].valid=false;
  bothMotorsOffSince=millis(); motor2LastRunEpoch=millis()/1000;
  lastRuntimeAccum=millis(); lastPowerCalc=millis(); throttleCheckTime=millis();

  addEventLog("System Boot","Power-On",0.0f);

  Serial.println(F("\n=========================================="));
  Serial.println(F("ACCESS:"));
  Serial.println(F("1. Phone WiFi -> SmartTank_AP / 12345678"));
  Serial.println(F("2. Browser -> http://192.168.4.1"));
  Serial.println(F("3. Admin Login -> jamalmd1"));
  Serial.println(F("4. WIFI tab -> Use HTTP form"));
  Serial.println(F("=========================================="));
  Serial.printf("Free Heap: %d\n\n", ESP.getFreeHeap());
}

void loop() {
  unsigned long now = millis();

  dnsServer.processNextRequest();
  ArduinoOTA.handle();
  server.handleClient();
  webSocket.loop();
  handleWiFiStateMachine();
  
  if (WiFi.status() == WL_CONNECTED) timeClient.update();
  yield();

  int reading=digitalRead(PIN_KILL_SW);
  if(reading!=lastKillState)lastKillDebounce=now;
  lastKillState=reading;
  if((now-lastKillDebounce)>DEBOUNCE_MS){
    if(reading!=killStableState){killStableState=reading;
      if(killStableState==LOW){killSwitchActive=true;
        if(motor1Running)setMotor1(false,"Kill Switch");
        if(motor2Running)setMotor2(false,"Kill Switch");
        addEventLog("KILL SWITCH","Hardware",tankLevelPercent);wakeOLED();}
      else killSwitchActive=false;}}

  if(now-lastSonicRead>=SONIC_READ_INTERVAL){
    lastSonicRead=now;
    digitalWrite(PIN_TRIG,LOW);delayMicroseconds(2);
    digitalWrite(PIN_TRIG,HIGH);delayMicroseconds(10);
    digitalWrite(PIN_TRIG,LOW);
    unsigned long dur=pulseIn(PIN_ECHO,HIGH,SENSOR_TIMEOUT_US);
    if(dur>0){
      float spd=(331.3f+0.606f*ambientTemp)/10000.0f;
      float dist=(dur*spd)/2.0f;
      if(dist>0&&dist<cfg.tankHeightCm+50){
        sonicBuffer[sonicIndex]=dist; sonicIndex=(sonicIndex+1)%SONIC_SAMPLES;
        if(!sonicBufferFull&&sonicIndex==0)sonicBufferFull=true;
        int cnt=sonicBufferFull?SONIC_SAMPLES:sonicIndex;
        if(cnt>=6){float sorted[SONIC_SAMPLES]; memcpy(sorted,sonicBuffer,sizeof(float)*cnt);
          for(int i=1;i<cnt;i++){float k=sorted[i];int j=i-1;while(j>=0&&sorted[j]>k){sorted[j+1]=sorted[j];j--;}sorted[j+1]=k;}
          float sum=0;int s=SONIC_TRIM_COUNT,e=cnt-SONIC_TRIM_COUNT;if(s>=e){s=0;e=cnt;}
          for(int i=s;i<e;i++)sum+=sorted[i];filteredDistance=sum/(e-s);}
        else{float sum=0;for(int i=0;i<cnt;i++)sum+=sonicBuffer[i];filteredDistance=sum/cnt;}
        if(filteredDistance<0)filteredDistance=0;if(filteredDistance>cfg.tankHeightCm)filteredDistance=cfg.tankHeightCm;
        tankLevelPercent=((cfg.tankHeightCm-filteredDistance)/cfg.tankHeightCm)*100.0f;
        if(tankLevelPercent<0)tankLevelPercent=0;if(tankLevelPercent>100)tankLevelPercent=100;
        tankVolumeLiters=(tankLevelPercent/100.0f)*cfg.tankMaxVolumeLiters; errorCodes[0]="";}
    }else{static int fc=0;fc++;if(fc>20){errorCodes[0]="ERR_01";fc=20;}}
  }

  if(!killSwitchActive&&!dryRunLocked){
    int mode=cfg.systemOperatingMode;
    if(mode==0){
      if(!motor1ManualOverride){
        if(!motor1Running&&tankLevelPercent>=(float)cfg.servoOnLevel){
          if(!isInSilentHours())setMotor1(true,"Auto Servo Open");}
        else if(motor1Running&&tankLevelPercent<(float)cfg.servoOffLevel){
          setMotor1(false,"Auto Servo Close");}}
      if(!motor2ManualOverride){
        if(isInMotor2Window()){
          if(!motor2Running&&tankLevelPercent<100.0f){if(!isInSilentHours())setMotor2(true,"Schedule");}
          else if(motor2Running&&tankLevelPercent>=100.0f)setMotor2(false,"Tank Full");}
        else{if(motor2Running&&!antiSeizeRunning)setMotor2(false,"Schedule End");}}
    }else if(mode==2){
      if(motor1Running&&!motor1ManualOverride)setMotor1(false,"Holiday");
      if(motor2Running&&!motor2ManualOverride&&!antiSeizeRunning)setMotor2(false,"Holiday");}
  }

  unsigned long dryMs=(unsigned long)cfg.dryRunTimeoutMinutes*60000UL;
  if(dryMs>0&&!dryRunLocked){
    if(motor1Running&&(now-motor1StartTime>=dryMs)){
      if(tankLevelPercent<motor1StartLevel+2.0f){setMotor1(false,"Dry Run");dryRunLocked=true;errorCodes[1]="ERR_02";wakeOLED();}
      else{motor1StartLevel=tankLevelPercent;motor1StartTime=now;}}
    if(motor2Running&&!antiSeizeRunning&&(now-motor2StartTime>=dryMs)){
      if(tankLevelPercent<motor2StartLevel+2.0f){setMotor2(false,"Dry Run");dryRunLocked=true;errorCodes[1]="ERR_02";wakeOLED();}
      else{motor2StartLevel=tankLevelPercent;motor2StartTime=now;}}}
  unsigned long maxMs=(unsigned long)cfg.maxSingleRunTimeoutMinutes*60000UL;
  if(maxMs>0){
    if(motor1Running&&(now-motor1StartTime>=maxMs)){setMotor1(false,"Max Runtime");errorCodes[3]="ERR_04";}
    if(motor2Running&&!antiSeizeRunning&&(now-motor2StartTime>=maxMs)){setMotor2(false,"Max Runtime");errorCodes[3]="ERR_04";}}
  if(!motor1Running&&!motor2Running){
    if(bothMotorsOffSince==0){bothMotorsOffSince=now;levelWhenBothOff=tankLevelPercent;}
    else if(now-bothMotorsOffSince>=LEAK_CHECK_WINDOW_MS){
      if(levelWhenBothOff-tankLevelPercent>=5.0f){leakageAlert=true;errorCodes[2]="ERR_03";
        addEventLog("LEAKAGE","Safety",tankLevelPercent);wakeOLED();}
      bothMotorsOffSince=now;levelWhenBothOff=tankLevelPercent;}
  }else bothMotorsOffSince=0;
  unsigned long nSec=now/1000;
  if(antiSeizeRunning){if(now-antiSeizeStart>=ANTI_SEIZE_RUN_TIME){setMotor2(false,"Anti-Seize Done");antiSeizeRunning=false;}}
  else if(nSec-motor2LastRunEpoch>=ANTI_SEIZE_INTERVAL){antiSeizeRunning=true;antiSeizeStart=now;setMotor2(true,"Anti-Seize");}

  if(motor1Running||motor2Running){
    if(now-throttleCheckTime>=120000UL){float g=tankLevelPercent-lastLevelForThrottle;
      lastLevelForThrottle=tankLevelPercent;throttleCheckTime=now;
      if(g<0.5f&&g>=0&&!pulseMode){pulseMode=true;pulsePhaseOn=true;pulseTimer=now;}
      else if(g>=1.0f&&pulseMode)pulseMode=false;}
    if(pulseMode){if(pulsePhaseOn&&(now-pulseTimer>=PULSE_ON_MS)){
        if(motor1Running)digitalWrite(RELAY_MOTOR1,RELAY_OFF);
        if(motor2Running)digitalWrite(RELAY_MOTOR2,RELAY_OFF);pulsePhaseOn=false;pulseTimer=now;}
      else if(!pulsePhaseOn&&(now-pulseTimer>=PULSE_OFF_MS)){
        if(motor1Running)digitalWrite(RELAY_MOTOR1,RELAY_ON);
        if(motor2Running)digitalWrite(RELAY_MOTOR2,RELAY_ON);pulsePhaseOn=true;pulseTimer=now;}}
  }else pulseMode=false;

  if(isInSilentHours()&&cfg.systemOperatingMode==0){
    if(motor1Running&&!motor1ManualOverride)setMotor1(false,"Silent");
    if(motor2Running&&!motor2ManualOverride&&!antiSeizeRunning)setMotor2(false,"Silent");}

  if(now-lastRuntimeAccum>=1000){if(motor1Running)motor1CumulativeRunSec++;
    if(motor2Running)motor2CumulativeRunSec++;lastRuntimeAccum=now;}
  if(now-lastPowerCalc>=60000){lastPowerCalc=now;int h=timeClient.getHours();float a=0;
    if(motor1Running)a+=1;if(motor2Running)a+=1;
    if(a>0&&h>=0&&h<24)hourlyKwh[h]+=(cfg.pumpWattage*a)/1000.0f/60.0f;}

  if(now-lastWSUpdate>=WS_UPDATE_INTERVAL){lastWSUpdate=now;broadcastWSData();}

  if(oledInitialized){if(!oledSleeping){if(now-lastOLEDActivity>=OLED_TIMEOUT_MS)sleepOLED();
    else{if(now-lastOledPageSwitch>=4000){oledPage=(oledPage+1)%3;lastOledPageSwitch=now;}updateOLED();}}}

  if(blynkEnabled && WiFi.status()==WL_CONNECTED && (now-lastBlynkRun>=100)){lastBlynkRun=now;runBlynk();}

  if(cfgDirty){saveConfig();cfgDirty=false;}
  yield();
}