// CONTROL.ino
extern void relaysAllOff();

// Nutrient-first timing
const unsigned long STABILIZE_MS = 1UL*60UL*1000UL; // 10 minutes
static unsigned long lastNutrientMs = 0;
static bool nutrientAdded = false;

// Pulse dosing to avoid overshoot
const unsigned long PULSE_ON_MS  = 2000;
const unsigned long PULSE_OFF_MS = 8000;
static unsigned long lastPulse = 0;
static bool pulseOn = false;

// pH safety pulse
const unsigned long PH_MAX_ON_MS = 5000;
static unsigned long phOnStart = 0;
static bool phOnActive = false;

void CONTROL(){
  // ---- TDS (nutrients A+B) first ----
  if (!isnan(tds)){
    if (tds >= set_tds_min && tds <= set_tds_max){
      digitalWrite(RELAY_NUTRIENT_A, RELAY_OFF);
      digitalWrite(RELAY_NUTRIENT_B, RELAY_OFF);
      stat_ppm = STATUS_IDEAL;  // in range
    } else if (tds < set_tds_min){
      unsigned long now = millis();
      if (now - lastPulse >= (pulseOn ? PULSE_ON_MS : PULSE_OFF_MS)){
        pulseOn = !pulseOn; 
        lastPulse = now;
      }
      if (pulseOn){
        digitalWrite(RELAY_NUTRIENT_A, RELAY_ON);
        digitalWrite(RELAY_NUTRIENT_B, RELAY_ON);
        nutrientAdded = true; 
        lastNutrientMs = now;
      } else {
        digitalWrite(RELAY_NUTRIENT_A, RELAY_OFF);
        digitalWrite(RELAY_NUTRIENT_B, RELAY_OFF);
      }
      stat_ppm = STATUS_LOW; // adding
    } else {
      digitalWrite(RELAY_NUTRIENT_A, RELAY_OFF);
      digitalWrite(RELAY_NUTRIENT_B, RELAY_OFF);
      stat_ppm = STATUS_HIGH; // too high
    }
  }

  // ---- Wait 10 min after nutrients before pH correction ----
  if (nutrientAdded && (millis() - lastNutrientMs < STABILIZE_MS)){
    digitalWrite(RELAY_PH_UP, RELAY_OFF);
    digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
    stat_ph = STATUS_UNKNOWN; // hold off pH changes while stabilizing
    return;
  }

  // ---- pH control (only if calibrated) ----
  if (!g_calibrated || isnan(ph)){
    digitalWrite(RELAY_PH_UP,   RELAY_OFF);
    digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
    stat_ph = STATUS_UNKNOWN;  // 0
    phOnActive = false;
    return;
  }

  // ---- pH control (short pulses for safety) ----
    if (ph >= set_ph_min && ph <= set_ph_max){
      digitalWrite(RELAY_PH_UP, RELAY_OFF);
      digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
      stat_ph = STATUS_IDEAL;
      phOnActive = false;
    } else if (ph < set_ph_min){
      digitalWrite(RELAY_PH_UP, RELAY_ON);
      digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
      stat_ph = STATUS_LOW;
      if(!phOnActive){ phOnStart = millis(); phOnActive = true; }
      if(millis() - phOnStart >= PH_MAX_ON_MS){
        digitalWrite(RELAY_PH_UP, RELAY_OFF);
        phOnActive = false;
      }
    } else { // ph > set_ph_max
      digitalWrite(RELAY_PH_UP, RELAY_OFF);
      digitalWrite(RELAY_PH_DOWN, RELAY_ON);
      stat_ph = STATUS_HIGH;
      if(!phOnActive){ phOnStart = millis(); phOnActive = true; }
      if(millis() - phOnStart >= PH_MAX_ON_MS){
        digitalWrite(RELAY_PH_DOWN, RELAY_OFF);
        phOnActive = false;
      }
    }
}
