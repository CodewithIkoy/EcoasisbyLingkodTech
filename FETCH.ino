#include <Arduino.h>
extern volatile bool g_calibrated;

// Return 0.0 if value is NaN/inf, otherwise pass it through
static inline double nz(double v) {
  return isfinite(v) ? v : 0.0;
}

// Milliseconds since epoch (if NTP time is available) else millis()
static inline int64_t nowMs() {
  time_t t;
  if (time(&t) > 0) return (int64_t)t * 1000LL;
  return (int64_t)millis();
}

// round to 3 decimals
static inline double r3(double v){ return isfinite(v) ? floor(v*1000.0 + 0.5)/1000.0 : 0.0; }

// ---- helpers to parse Firestore JSON payload ----
static bool jsonGet(FirebaseJson &j, const String &path, FirebaseJsonData &out){
  return j.get(out, path);
}

static double readNumField(FirebaseJson &j, const char* base){
  FirebaseJsonData d;

  // Try doubleValue first
  String p = String(base) + "/doubleValue";
  if (jsonGet(j, p, d)) return d.to<double>();

  // Fallback to integerValue
  p = String(base) + "/integerValue";
  if (jsonGet(j, p, d)) return (double) d.to<long>();

  return NAN;
}

static bool readBoolField(FirebaseJson &j, const String &path){
  FirebaseJsonData d;
  if (jsonGet(j, path, d)) return d.to<bool>();
  return false;
}

// ===============================
// NOTIFICATION FUNCTIONS - Firestore Only
// ===============================

static int prev_stat_ph = 2;  
static int prev_stat_ppm = 2;

// Simplified notification function - only writes to Firestore
static void sendNotification(const String& title, const String& content) {
    // Create notification document in Firestore
    String docPath = "notifications/" + String(nowMs());
    FirebaseJson notification;
    notification.set("fields/title/stringValue", title);
    notification.set("fields/content/stringValue", content);
    notification.set("fields/timestamp/timestampValue", nowRfc3339());
    notification.set("fields/read/booleanValue", false); // Mark as unread
    notification.set("fields/type/stringValue", "system_alert");
    
    if (Firebase.Firestore.createDocument(&fbdoFS, PROJECT_ID, "", docPath.c_str(), notification.raw())) {
        Serial.printf("[NOTIFICATION] Sent to Firestore: %s - %s\n", title.c_str(), content.c_str());
    } else {
        Serial.printf("[NOTIFICATION] Failed to send to Firestore: %s\n", fbdoFS.errorReason().c_str());
    }
}

static void checkAndNotifyStatus() {
  // Check pH status changes
  if (stat_ph != 2 && prev_stat_ph == 2) {
    String title, content;
    if (stat_ph == 1) {
      title = "pH Level Too Low";
      content = "pH level is below the recommended range. Current pH: " + String(ph, 2);
    } else if (stat_ph == 3) {
      title = "pH Level Too High";
      content = "pH level is above the recommended range. Current pH: " + String(ph, 2);
    } else {
      title = "pH Level Out of Range";
      content = "pH level is outside recommended range. Current pH: " + String(ph, 2);
    }
    sendNotification(title, content);
  }
  
  // Check PPM status changes
  if (stat_ppm != 2 && prev_stat_ppm == 2) {
    String title, content;
    if (stat_ppm == 1) {
      title = "Nutrient Level Too Low";
      content = "Nutrient concentration (PPM) is below recommended range. Current PPM: " + String(tds, 0);
    } else if (stat_ppm == 3) {
      title = "Nutrient Level Too High";
      content = "Nutrient concentration (PPM) is above recommended range. Current PPM: " + String(tds, 0);
    } else {
      title = "Nutrient Level Out of Range";
      content = "Nutrient concentration (PPM) is outside recommended range. Current PPM: " + String(tds, 0);
    }
    sendNotification(title, content);
  }
  
  // Check recovery notifications
  if (stat_ph == 2 && prev_stat_ph != 2) {
    String title = "pH Level Normalized";
    String content = "pH level has returned to normal range. Current pH: " + String(ph, 2);
    sendNotification(title, content);
  }
  
  if (stat_ppm == 2 && prev_stat_ppm != 2) {
    String title = "Nutrient Level Normalized";
    String content = "Nutrient concentration (PPM) has returned to normal range. Current PPM: " + String(tds, 0);
    sendNotification(title, content);
  }
  
  // Update previous states
  prev_stat_ph = stat_ph;
  prev_stat_ppm = stat_ppm;
}

// ===============================
// SETTINGS (ecoasis/settings)
// ===============================
void fetchSettings(){
  if (!Firebase.Firestore.getDocument(&fbdoFS, PROJECT_ID, "", "ecoasis/settings")) return;
  FirebaseJson j; j.setJsonData(fbdoFS.payload().c_str());

  double v;
  if (!isnan(v = readNumField(j, "fields/ph_min")))  set_ph_min  = v;
  if (!isnan(v = readNumField(j, "fields/ph_max")))  set_ph_max  = v;
  if (!isnan(v = readNumField(j, "fields/tds_min"))) set_tds_min = v;
  if (!isnan(v = readNumField(j, "fields/tds_max"))) set_tds_max = v;
}

// One-shot strict fetch used at boot (requires all 4 present)
bool fetchSettingsOnceFS(){
  if (!Firebase.Firestore.getDocument(&fbdoFS, PROJECT_ID, "", "ecoasis/settings")) {
    Serial.println("[FS] settings get failed");
    return false;
  }
  FirebaseJson j; j.setJsonData(fbdoFS.payload().c_str());
  double pmin = readNumField(j, "fields/ph_min");
  double pmax = readNumField(j, "fields/ph_max");
  double tmin = readNumField(j, "fields/tds_min");
  double tmax = readNumField(j, "fields/tds_max");
  if (isnan(pmin) || isnan(pmax) || isnan(tmin) || isnan(tmax)) {
    Serial.println("[FS] settings incomplete (need ph_min,ph_max,tds_min,tds_max)");
    return false;
  }
  set_ph_min  = pmin;  
  set_ph_max  = pmax;
  set_tds_min = tmin;  
  set_tds_max = tmax;
  return true;
}

// ===============================
// CALIBRATION (ecoasis/calibration)
// ===============================

static void saveCalToPrefs(const char* key, float v) {
  prefs.begin("phCal", false);
  prefs.putFloat(key, v);
  prefs.end();
}

static bool saneVoltage(float v) {
  return (v > 0.05f && v < 4.95f);
}

// Update Firestore "complete" flag only
static void updateCalCompleteFlagFS() {
  FirebaseJson p;
  bool complete = (saneVoltage(buff_ph4) &&
                   saneVoltage(buff_ph7) &&
                   saneVoltage(buff_ph9));
  p.set("fields/complete/booleanValue", complete);
  Firebase.Firestore.patchDocument(&fbdoFS, PROJECT_ID, "",
                                   "ecoasis/calibration", p.raw(),
                                   "complete");
  g_calibrated = complete;
}

// Listener-style handler: ESP32 no longer writes values
void handleCalibrationRequests() {
  if (!Firebase.Firestore.getDocument(&fbdoFS, PROJECT_ID, "",
                                      "ecoasis/calibration"))
    return;

  FirebaseJson j;
  j.setJsonData(fbdoFS.payload().c_str());

  // Just read values (app is responsible for writing them)
  double v4 = readNumField(j, "fields/values/mapValue/fields/ph4");
  double v7 = readNumField(j, "fields/values/mapValue/fields/ph7");
  double v9 = readNumField(j, "fields/values/mapValue/fields/ph9");

  if (saneVoltage(v4)) buff_ph4 = (float)v4;
  if (saneVoltage(v7)) buff_ph7 = (float)v7;
  if (saneVoltage(v9)) buff_ph9 = (float)v9;

  updateCalCompleteFlagFS(); // set g_calibrated + Firestore flag
}

// STRICT 3-point fetch: all must exist and be sane
bool fetchCalibrationOnceFS() {
  if (!Firebase.Firestore.getDocument(&fbdoFS, PROJECT_ID, "",
                                      "ecoasis/calibration")) {
    Serial.println("[FS] calibration fetch failed");
    g_calibrated = false;
    return false;
  }

  FirebaseJson j;
  j.setJsonData(fbdoFS.payload().c_str());

  double v4 = readNumField(j, "fields/values/mapValue/fields/ph4");
  double v7 = readNumField(j, "fields/values/mapValue/fields/ph7");
  double v9 = readNumField(j, "fields/values/mapValue/fields/ph9");

  if (!(saneVoltage(v4) && saneVoltage(v7) && saneVoltage(v9))) {
    Serial.println("[FS] calibration incomplete (need valid ph4, ph7, ph9)");
    g_calibrated = false;
    return false;
  }

  // Load into working buffers
  buff_ph4 = (float)v4;
  buff_ph7 = (float)v7;
  buff_ph9 = (float)v9;
  cal_high_pH = 9.0f;

  // Persist to NVS (so reboot keeps them)
  saveCalToPrefs("ph4", buff_ph4);
  saveCalToPrefs("ph7", buff_ph7);
  saveCalToPrefs("ph9", buff_ph9);

  // Mark complete in Firestore
  FirebaseJson p;
  p.set("fields/complete/booleanValue", true);
  p.set("fields/high_ref/integerValue", 9);
  Firebase.Firestore.patchDocument(&fbdoFS, PROJECT_ID, "",
                                   "ecoasis/calibration", p.raw(),
                                   "complete,high_ref");

  g_calibrated = true;
  Serial.printf("[FS] calibration loaded: ph4=%.3f, ph7=%.3f, ph9=%.3f\n",
                buff_ph4, buff_ph7, buff_ph9);
  return true;
}

bool waitForFSCalibration(uint32_t maxWaitMs) {
  uint32_t start = millis(), lastLog = 0;
  while (millis() - start < maxWaitMs) {
    if (fetchCalibrationOnceFS()) return true;
    if (millis() - lastLog > 3000) {
      Serial.println("[BOOT] Waiting for ph4/ph7/ph9 in Firestore...");
      lastLog = millis();
    }
    delay(600);  // gentle poll
  }
  return false;
}

bool blockingInitialSync(uint32_t maxWaitMs) {
  uint32_t start = millis();
  bool settingsOK = false, calOK = false;

  while (millis() - start < maxWaitMs) {
    if (!settingsOK) settingsOK = fetchSettingsOnceFS();
    if (!calOK) calOK = fetchCalibrationOnceFS(); // STRICT 3-pt
    if (settingsOK && calOK) return true;
    delay(400);
  }
  return false; // require both before starting system
}

// rounders
static inline double r0(double v){ return isfinite(v) ? (double)llround(v)           : 0.0; } // 0 dp
static inline double r1(double v){ return isfinite(v) ? floor(v*10.0  + 0.5)/10.0   : 0.0; } // 1 dp
static inline double r2(double v){ return isfinite(v) ? floor(v*100.0 + 0.5)/100.0  : 0.0; } // 2 dp

// RFC3339 UTC for Firestore timestampValue
static String nowRfc3339() {
  time_t t = time(nullptr);
  if (t <= 0) return String("1970-01-01T00:00:00Z");
  struct tm tm; gmtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return String(buf);
}

static bool fs_patch_or_create(const char* docPath, FirebaseJson& content, const char* mask) {
  if (Firebase.Firestore.patchDocument(&fbdoFS, PROJECT_ID, "", docPath, content.raw(), mask)) return true;
  return Firebase.Firestore.createDocument(&fbdoFS, PROJECT_ID, "", docPath, content.raw());
}

void pushReadingsAndStatus() {
  // ---------- ecoasis/readings ----------
  const char* readingsDoc = "ecoasis/readings";

  FirebaseJson content;
  content.set("fields/air/doubleValue",    r1(airTemp));   // °C, 1 dp
  content.set("fields/h2o/doubleValue",    r1(temp));      // °C, 1 dp
  content.set("fields/humid/doubleValue",  r1(humidity));  // %,  1 dp
  content.set("fields/lux/doubleValue",    r0(light));     // int-like
  content.set("fields/ph/doubleValue",     r2(ph));        // pH, 2 dp
  content.set("fields/ppm/doubleValue",    r0(tds));       // ppm, integer
  content.set("fields/a/doubleValue",      r0(tank1));
  content.set("fields/b/doubleValue",      r0(tank2));
  content.set("fields/down/doubleValue",   r0(tank3));
  content.set("fields/up/doubleValue",     r0(tank4));
  content.set("fields/ph_v/doubleValue", r3(voltage_ph));

  // Store real Firestore timestamp (includes date)
  content.set("fields/timestamp/timestampValue", nowRfc3339());

  // status map (1=low, 2=ideal, 3=high)
  content.set("fields/status/mapValue/fields/ph/integerValue",  (int)stat_ph);
  content.set("fields/status/mapValue/fields/tds/integerValue", (int)stat_ppm);
  content.set("fields/status/mapValue/fields/calibrated/booleanValue", g_calibrated);

  const char* readingsMask =
    "air,h2o,humid,lux,ph,ppm,a,b,down,up,ph_v,timestamp,status.ph,status.tds";

  fs_patch_or_create(readingsDoc, content, readingsMask);

  // ---------- Check and send notifications ----------
  checkAndNotifyStatus();

  // ---------- ecoasis/status (pumps live state) ----------
  FirebaseJson pumps;
  pumps.set("fields/pump_ph_up/booleanValue",   (digitalRead(RELAY_PH_UP)      == RELAY_ON));
  pumps.set("fields/pump_ph_down/booleanValue", (digitalRead(RELAY_PH_DOWN)    == RELAY_ON));
  pumps.set("fields/pump_a/booleanValue",       (digitalRead(RELAY_NUTRIENT_A) == RELAY_ON));
  pumps.set("fields/pump_b/booleanValue",       (digitalRead(RELAY_NUTRIENT_B) == RELAY_ON));
  const char* pumpsMask = "pump_ph_up,pump_ph_down,pump_a,pump_b";
  fs_patch_or_create("ecoasis/status", pumps, pumpsMask);
}