// SENSOR.ino
extern volatile bool g_calibrated;
static inline bool validV(float v){ return v > 0.05f && v < 4.95f; }

float phFromVoltage(float v, float tC){
  if (!(validV(buff_ph4)&&validV(buff_ph7)&&validV(buff_ph9))) return NAN;

  // assume V4 > V7 > V9 (typical pH boards). If yours differ, sort by voltage.
  float p;
  if (v >= buff_ph7) {
    // between pH4 and pH7
    float m = (7.0f - 4.0f)/(buff_ph7 - buff_ph4);
    p = 4.0f + m*(v - buff_ph4);
  } else {
    // between pH7 and pH9
    float m = (9.0f - 7.0f)/(buff_ph9 - buff_ph7);
    p = 7.0f + m*(v - buff_ph7);
  }

  // Optional (usually not needed): slope temperature scaling around pH7
  // float scale = (tC + 273.15f) / 298.15f;   // 298.15K = 25°C
  // p = 7.0f + (p - 7.0f) * scale;

  return constrain(p, 0.0f, 14.0f);
}

// ===== DS18B20 (water) =====
void SENSOR_SUHU(){
  wTemp.requestTemperatures();
  float t = wTemp.getTempCByIndex(0);
  temp = (t < -50 || t > 125) ? NAN : t;
}

// ===== DHT22 (air) =====
void SENSOR_AIR(){
  airTemp  = dht.readTemperature();
  humidity = dht.readHumidity();
}

// Choose a gain that isn't near rails, then read once
static float readPhVoltsAuto() {
  const adsGain_t gains[4] = { GAIN_FOUR, GAIN_TWO, GAIN_ONE, GAIN_TWOTHIRDS };
  const float     fs  [4]  = { 1.024f,    2.048f,   4.096f,   6.144f };
  for (int i = 0; i < 4; ++i) {
    ads.setGain(gains[i]);
    delay(2);
    int16_t raw = ads.readADC_SingleEnded(0);
    float v = ads.computeVolts(raw);
    if (fabs(v) < fs[i]*0.98f) return v; // not clipped
  }
  return ads.computeVolts(ads.readADC_SingleEnded(0)); // last reading
}

void SENSOR_PH(){
  float v   = readPhVoltsAuto();   // <-- no clipping
  voltage_ph = v;
  float tC  = isnan(temp) ? 25.0f : temp;
  ph = phFromVoltage(v, tC);
  Serial.printf("pH Info -> V: %.3f | Tw: %.1f | pH: %.2f\n", v, tC, ph);
}


// ===== TDS (ADS1115 A1) =====
void SENSOR_TDS() {
  int16_t adc1 = ads.readADC_SingleEnded(1);
  float voltage = countsToVolts(adc1);      // <-- use your helper

  float tC  = isnan(temp) ? 25.0f : temp;
  float tcf = 1.0f + 0.02f * (tC - 25.0f);

  float ec = (133.42f*voltage*voltage*voltage
            - 255.86f*voltage*voltage
            + 857.39f*voltage) * 0.5f;

  tds = ec / tcf;
  if (tds < 0) tds = 0;
  if (tds > 2000) tds = 2000;
}

// Optional UI/randoms
void SENSOR_RANDOM(){
  light = random(300, 1200);
  // if you want real tank levels, wire sensors and replace these:
  // leaving randoms to populate your front-end in the meantime
  extern float tank1, tank2, tank3, tank4;
  tank1 = random(0, 100);
  tank2 = random(0, 100);
  tank3 = random(0, 100);
  tank4 = random(0, 100);
}
