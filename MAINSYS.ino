// MAINSYS.ino
void SYSTEM_READ_SENSOR(void* parameter);
void SYSTEM_MAIN_CONTROL(void* parameter);
void SYSTEM_FETCH_DATA(void* parameter);

void RUN_SYSTEM(){
  xTaskCreate(SYSTEM_READ_SENSOR,  "READ",  5000, NULL, 1, NULL);
  xTaskCreate(SYSTEM_MAIN_CONTROL, "CTRL",  6000, NULL, 1, NULL);
  xTaskCreate(SYSTEM_FETCH_DATA,   "FETCH", 6000, NULL, 1, NULL);
}

// Declared in FETCH_FS.ino
extern void fetchSettings();
extern void pushReadingsAndStatus();
extern void handleCalibrationRequests();

void SYSTEM_READ_SENSOR(void*){
  for(;;){
    SENSOR_SUHU();
    SENSOR_AIR();
    SENSOR_PH();
    SENSOR_TDS();
    SENSOR_RANDOM();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
void SYSTEM_MAIN_CONTROL(void*){
  for(;;){
    CONTROL();
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
void SYSTEM_FETCH_DATA(void*){
  for(;;){
    pushReadingsAndStatus();
    fetchSettings();
    handleCalibrationRequests();
    vTaskDelay(pdMS_TO_TICKS(6000));
  }
}

extern bool fetchSettingsOnceFS();
extern bool fetchCalibrationOnceFS();

void BOOT_FETCH(void*){
  Serial.println("[BOOT] Waiting for Firestore settings & calibration (will keep retrying)...");
  const uint32_t LOG_EVERY_MS = 10000;      // log every 10s
  uint32_t lastLog = 0;

  uint32_t retryDelayMs = 10000;             // start with 10s retry
  const uint32_t TWO_MIN_MS = 120000;       // 2 minutes
  uint32_t startWait = millis();

  bool settingsOK = false, calOK = false;

  for(;;){
    // Try to fetch; once true, keep it true
    settingsOK = fetchSettingsOnceFS()     || settingsOK;
    calOK      = fetchCalibrationOnceFS()  || calOK;

    if (settingsOK && calOK) {
      Serial.println("[BOOT] Firestore ready (settings + calibration). Starting system...");
      break;
    }

    if (millis() - lastLog >= LOG_EVERY_MS) {
      Serial.printf("[BOOT] still waiting... settings=%s, calibration=%s\n",
                    settingsOK ? "OK" : "—", calOK ? "OK" : "—");
      lastLog = millis();
    }

    // After 2 minutes, slow retries to be kinder to network/Firestore
    if (millis() - startWait >= TWO_MIN_MS) {
      retryDelayMs = 10000; // 10s
    }

    delay(retryDelayMs);
  }
}
