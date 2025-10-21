// ecoasis_final (with OLED-mirrored logging)
// ------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "DHT.h"
#include <Preferences.h>
#include <time.h>
#include <math.h>

// ==== OLED (128x64 SSD1306) ====
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);



// ---- Logging helpers: mirror to Serial + OLED ----
static int oledX = 0, oledY = 0;
static inline void oledNewline() {
  oledY += 8; oledX = 0;
  if (oledY > 56) { display.clearDisplay(); oledY = 0; }
}
void logMessage(const String &msg, bool newline=true) {
  if (newline) Serial.println(msg); else Serial.print(msg);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(oledX, oledY);
  if (newline) { display.println(msg); display.display(); oledNewline(); }
  else         { display.print(msg);  display.display(); oledX = display.getCursorX(); }
}
// printf-style logging
void logf(bool newline, const char *fmt, ...) {
  char buf[192];
  va_list args; va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logMessage(String(buf), newline);
}
void logln(const String &msg){ logMessage(msg, true); }
void log(const String &msg){ logMessage(msg, false); }

// ==== EDIT THESE ====
#define WIFI_SSID      "DITO_DB460_2.4"
#define WIFI_PASSWORD  "5AaC4752"
#define API_KEY        "AIzaSyDQ19-KGnThcelmc10yRS8iPWy0ymKQ3kI"
#define PROJECT_ID     "ecoasis-bf70b"   

// ==== Firebase Firestore only ====
FirebaseData fbdoFS;
FirebaseAuth auth;
FirebaseConfig fbconfig;

// ==== ADS1115 ====
Adafruit_ADS1115 ads;
const float ADS_LSB_mV = 0.1875f;
float countsToVolts(int16_t c){ return (c * ADS_LSB_mV) / 1000.0f; }

// Global calibration flag
volatile bool g_calibrated = false;

// ==== DS18B20 ====
#define ONE_WIRE_PIN 4
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature wTemp(&oneWire);

// ==== DHT22 ====
#define DHTPIN 32
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ==== Relays (Active LOW) ====
#define RELAY_PH_UP       26
#define RELAY_PH_DOWN     27
#define RELAY_NUTRIENT_A  25
#define RELAY_NUTRIENT_B  14
#define RELAY_ON  LOW
#define RELAY_OFF HIGH
void relaysAllOff(){
  digitalWrite(RELAY_PH_UP, RELAY_OFF);
  digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
  digitalWrite(RELAY_NUTRIENT_A, RELAY_OFF);
  digitalWrite(RELAY_NUTRIENT_B, RELAY_OFF);
}

// ==== Preferences for pH cal ====
Preferences prefs;

// -------- GLOBALS --------
float airTemp=0, temp=0, humidity=0, light=0, ph=0, tds=0;
float tank1=0, tank2=0, tank3=0, tank4=0;
float buff_ph4=0, buff_ph7=0, buff_ph9=0, voltage_ph=0;
float set_ph_min=0, set_ph_max=0, set_tds_min=0, set_tds_max=0;
enum Status: uint8_t { STATUS_UNKNOWN=0, STATUS_IDEAL=2, STATUS_LOW=1, STATUS_HIGH=3 };
Status stat_ph=STATUS_UNKNOWN, stat_ppm=STATUS_UNKNOWN;
float cal_high_pH = 9.0f;

// Prototypes
void RUN_SYSTEM();
void BOOT_FETCH(void*);  // forward declare a small background fetch task

// ---- WiFi ----
void connectWiFi(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  logf(false, "Connecting to %s", WIFI_SSID);
  for(int i=0;i<60 && WiFi.status()!=WL_CONNECTED;i++){
    delay(500);
    log(".");
  }
  logln(WiFi.status()==WL_CONNECTED ? "\nWiFi OK" : "\nWiFi failed");
}

// ---- Firebase ----
static void initFirebase(){
  fbconfig.api_key = API_KEY;

  // NTP for timestamps (optional)
  configTime(0,0,"pool.ntp.org","time.nist.gov");

  // Anonymous auth so Firestore has an ID token
  if (Firebase.signUp(&fbconfig, &auth, "", "")) {
    logln("Firebase anonymous sign-in OK");
  } else {
    logf(true, "signUp error: %s", fbdoFS.errorReason().c_str());
  }
  Firebase.begin(&fbconfig, &auth);
  Firebase.reconnectWiFi(true);
  logln("Firebase init complete");
}

// ---- IO ----
static void initIO(){
  pinMode(RELAY_PH_UP, OUTPUT);
  pinMode(RELAY_PH_DOWN, OUTPUT);
  pinMode(RELAY_NUTRIENT_A, OUTPUT);
  pinMode(RELAY_NUTRIENT_B, OUTPUT);
  relaysAllOff();
  logln("Relays initialized (all OFF)");
}

// Force all relays OFF as early as possible (active-LOW safe)
static void safeBootOutputs() {
  pinMode(RELAY_PH_UP, OUTPUT);
  pinMode(RELAY_PH_DOWN, OUTPUT);
  pinMode(RELAY_NUTRIENT_A, OUTPUT);
  pinMode(RELAY_NUTRIENT_B, OUTPUT);

  digitalWrite(RELAY_PH_UP, RELAY_OFF);
  digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
  digitalWrite(RELAY_NUTRIENT_A, RELAY_OFF);
  digitalWrite(RELAY_NUTRIENT_B, RELAY_OFF);
}

// ---- Sensors ----
static void initSensors(){
  Wire.begin(21,22);

  // OLED comes up here to ensure Wire is ready
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    Serial.println("SSD1306 allocation failed"); // last resort if OLED is dead
    for(;;){ delay(1000); }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Ecoasis Booting...");
  display.display();
  delay(600);

  if(!ads.begin(0x48,&Wire)){
    logln("ADS1115 missing");
    while(1){ delay(1000); }
  }
  ads.setGain(GAIN_TWO);
  logln("ADS1115 OK (GAIN_TWO)");

  wTemp.begin(); 
  wTemp.setWaitForConversion(false);
  logln("DS18B20 OK");

  dht.begin();
  logln("DHT22 OK");

  prefs.begin("phCal", true);
  buff_ph4  = prefs.getFloat("ph4",  0);
  buff_ph7  = prefs.getFloat("ph7",  0);
  buff_ph9  = prefs.getFloat("ph9",  0);
  prefs.end();
  logf(true, "Loaded pH cal volts: 4=%.3f 7=%.3f 9=%.3f", buff_ph4, buff_ph7, buff_ph9);
}

void setup(){
  Serial.begin(115200);

  safeBootOutputs();

  // Prepare OLED immediately (minimal splash if Wire not yet set)
  // (Full init occurs in initSensors after Wire.begin)
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println("Starting..."); display.display();

  connectWiFi();
  initFirebase();
  initIO();
  initSensors();

  logln("[BOOT] Waiting 60s before system run...");
  for(int i=30; i>0; i--){
    logf(true, "Start in %d sec", i);
    delay(1000); // 1 second countdown
  }

  // Start runtime tasks after wait
  RUN_SYSTEM();

  logln("[BOOT] System started. pH sensor running; Firestore fetch in background...");
}

void loop(){
  vTaskDelay(pdMS_TO_TICKS(5000));
} 
