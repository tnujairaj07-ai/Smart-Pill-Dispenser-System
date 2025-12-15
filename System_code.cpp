#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_DEVICE_NAME   "Pill Dispenser"
#define BLYNK_AUTH_TOKEN    "YOUR_AUTH_TOKEN"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <RTClib.h>

// WiFi credentials
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

// --- Hardware Pins ---
const int servoPins[]   = {13, 12};   // Slot 1, Slot 2
const int vibPin        = 33;
const int irPin         = 32;
const int buzzerPin     = 19;
const int redLedPin     = 26;
const int greenLedPin   = 18;

// --- LCD, RTC, Servo Setup ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
Servo servos[sizeof(servoPins) / sizeof(servoPins[0])];

// --- Servo Positions ---
const int SERVO_HOME      = 180;
const int SERVO_DISPENSE  = 0;
const int SERVO_RETURN    = 180;

// --- Detection & Retry ---
const unsigned long VIBRATION_TIMEOUT_MS   = 2000;   // 2s to detect pill drop
const unsigned long PILL_PICKUP_TIMEOUT_MS = 60000;  // 60s total window for outlet action
const int           MAX_RETRIES            = 6;

// --- Message display times ---
const unsigned long MESSAGE_DISPLAY_MS = 2000; // Each message shows ≥ 2s

// --- Schedule (RTC based) ---
struct DispenseSchedule {
  uint8_t hour;
  uint8_t minute;
  uint8_t slot;
  bool    enabled;
};

DispenseSchedule scheduleTimes[] = {
  {8,  0, 0, true},   // 08:00, Slot 1
  {20, 0, 1, true}    // 20:00, Slot 2
};
const int SCHEDULE_COUNT = sizeof(scheduleTimes) / sizeof(DispenseSchedule);
bool scheduleTriggered[SCHEDULE_COUNT] = {false, false};

// --- System control from Blynk ---
bool systemEnabled = true;

// --- LCD Double Buffer ---
char lastLine1[17] = "";
char lastLine2[17] = "";
void lcdShowStatus(const char *line1, const char *line2, unsigned long showMs = MESSAGE_DISPLAY_MS) {
  if (strncmp(lastLine1, line1, 16) != 0) {
    lcd.setCursor(0, 0); lcd.print("                ");
    lcd.setCursor(0, 0); lcd.print(line1);
    strncpy(lastLine1, line1, 16);
  }
  if (strncmp(lastLine2, line2, 16) != 0) {
    lcd.setCursor(0, 1); lcd.print("                ");
    lcd.setCursor(0, 1); lcd.print(line2);
    strncpy(lastLine2, line2, 16);
  }
  if (showMs > 0) delay(showMs);
}

// --- Blynk helpers ---
// V0: Status display, V5: event log
void blynkStatus(const String &msg) {
  Serial.println(msg);
  Blynk.virtualWrite(V0, msg);
}

void blynkLog(const String &msg) {
  Serial.println(msg);
  Blynk.virtualWrite(V5, msg);
  Blynk.logEvent("dispenser_event", msg);
}

// --- Beep and LED helpers ---
void beep(int times = 1, int ms = 200) {
  for (int i = 0; i < times; i++) {
    digitalWrite(buzzerPin, HIGH);
    delay(ms);
    digitalWrite(buzzerPin, LOW);
    delay(150);
  }
}

void setLed(bool greenOn, bool redOn) {
  digitalWrite(greenLedPin, greenOn ? HIGH : LOW);
  digitalWrite(redLedPin,  redOn  ? HIGH : LOW);
}

// --- Wait for vibration (pill dropped) ---
bool waitForVibration(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (digitalRead(vibPin) == HIGH) {
      Serial.println("Vibration detected: pill dropped");
      return true;
    }
    delay(30);
  }
  Serial.println("No vibration detected within timeout");
  return false;
}

// --- Wait for outlet cycle: LOW (closed with pill) then HIGH (opened / pill taken) ---
bool waitForOutletCycle(unsigned long timeoutMs) {
  unsigned long start = millis();
  bool seenClosed = false;

  while (millis() - start < timeoutMs) {
    int irState = digitalRead(irPin);

    if (!seenClosed) {
      // First wait for outlet to be closed (IR LOW)
      if (irState == LOW) {
        seenClosed = true;
        Serial.println("IR: outlet closed / pill present (LOW detected)");
      }
    } else {
      // After closed, wait for outlet to be opened (IR HIGH)
      if (irState == HIGH) {
        Serial.println("IR: outlet opened / pill taken (HIGH after LOW)");
        return true;
      }
    }

    delay(120);
  }

  Serial.println("IR: outlet LOW->HIGH sequence NOT completed in timeout");
  return false;
}

// --- Greeting based on hour ---
const char* greetingForHour(int hour) {
  if (hour >= 5  && hour < 12) return "Good Morning";
  if (hour >= 12 && hour < 17) return "Good Afternoon";
  if (hour >= 17 && hour < 21) return "Good Evening";
  return "Good Night";
}

// --- Main dispensing logic ---
void dispenseSlot(int slot) {
  if (slot < 0 || slot >= (int)(sizeof(servoPins) / sizeof(servoPins[0]))) return;

  char buf1[17], buf2[17];
  snprintf(buf1, sizeof(buf1), "Slot %d Ready", slot + 1);
  lcdShowStatus(buf1, "Dispensing...", 2000);
  setLed(false, false);
  beep(1, 200);
  blynkStatus("Dispensing slot " + String(slot + 1));
  blynkLog("Dispense start for slot " + String(slot + 1));

  bool dispensed = false;
  int attempt = 0;

  // 1) Try until vibration confirms pill drop
  while (!dispensed && attempt < MAX_RETRIES) {
    attempt++;

    servos[slot].write(SERVO_DISPENSE);
    delay(1000);
    servos[slot].write(SERVO_RETURN);
    delay(1000);
    servos[slot].write(SERVO_HOME);
    delay(1000);

    bool vib = waitForVibration(VIBRATION_TIMEOUT_MS);
    if (vib) {
      dispensed = true;
      lcdShowStatus(buf1, "Pill dispensed!", 2000);
      setLed(true, false);
      beep(1, 250);
      blynkLog("Pill dispensed (slot " + String(slot + 1) + ")");
    } else {
      snprintf(buf2, sizeof(buf2), "Retry %d/%d", attempt, MAX_RETRIES);
      lcdShowStatus("No vibration", buf2, 2000);
      setLed(false, true);
      beep(2, 180);
      blynkLog("No vibration, retry " + String(attempt) + " (slot " + String(slot + 1) + ")");
    }
  }

  if (!dispensed) {
    lcdShowStatus("DISPENSE ERROR!", buf1, 3000);
    setLed(false, true);
    beep(4, 250);
    servos[slot].write(SERVO_HOME);
    delay(1000);
    setLed(false, false);
    blynkStatus("DISPENSE ERROR slot " + String(slot + 1));
    blynkLog("CRITICAL: dispense failed after retries (slot " + String(slot + 1) + ")");
    Blynk.logEvent("dispense_failed", "Slot " + String(slot + 1) + " failed to dispense");
    return;
  }

  // 2) After successful dispensing, wait for outlet to be used (LOW->HIGH sequence)
  lcdShowStatus("Take your pill", "Waiting...", 2000);
  blynkStatus("Pill ready in slot " + String(slot + 1));
  blynkLog("Waiting for outlet action (slot " + String(slot + 1) + ")");

  bool taken = waitForOutletCycle(PILL_PICKUP_TIMEOUT_MS);

  if (taken) {
    lcdShowStatus("Pill taken", "Thank you!", 2500);
    setLed(false, false);
    beep(2, 180);
    blynkStatus("Pill taken - slot " + String(slot + 1));
    blynkLog("Pill taken by user (slot " + String(slot + 1) + ")");
  } else {
    lcdShowStatus("ALERT!", "Pill not taken!", 3000);
    setLed(false, true);
    beep(3, 250);
    blynkStatus("ALERT: pill not taken (slot " + String(slot + 1) + ")");
    blynkLog("ALERT: pill NOT taken in time (slot " + String(slot + 1) + ")");
    Blynk.logEvent("pill_not_taken", "Patient did not take pill from slot " + String(slot + 1));
  }

  servos[slot].write(SERVO_HOME);
  setLed(false, false);
  delay(1000);
}

// ---------------- BLYNK HANDLERS ----------------

// V1: manual dispense Slot 1
BLYNK_WRITE(V1) {
  int val = param.asInt();
  if (val == 1 && systemEnabled) {
    blynkLog("Manual dispense requested: slot 1");
    dispenseSlot(0);
    Blynk.virtualWrite(V1, 0);
  }
}

// V2: manual dispense Slot 2
BLYNK_WRITE(V2) {
  int val = param.asInt();
  if (val == 1 && systemEnabled) {
    blynkLog("Manual dispense requested: slot 2");
    dispenseSlot(1);
    Blynk.virtualWrite(V2, 0);
  }
}

// V3: schedule slot 1 – format "HH:MM"
BLYNK_WRITE(V3) {
  String s = param.asStr();
  int colon = s.indexOf(':');
  if (colon > 0) {
    int h = s.substring(0, colon).toInt();
    int m = s.substring(colon + 1).toInt();
    scheduleTimes[0].hour    = (uint8_t)h;
    scheduleTimes[0].minute  = (uint8_t)m;
    scheduleTimes[0].slot    = 0;
    scheduleTimes[0].enabled = true;
    blynkLog("Schedule updated: Slot 1 -> " + s);
  }
}

// V4: schedule slot 2 – format "HH:MM"
BLYNK_WRITE(V4) {
  String s = param.asStr();
  int colon = s.indexOf(':');
  if (colon > 0) {
    int h = s.substring(0, colon).toInt();
    int m = s.substring(colon + 1).toInt();
    scheduleTimes[1].hour    = (uint8_t)h;
    scheduleTimes[1].minute  = (uint8_t)m;
    scheduleTimes[1].slot    = 1;
    scheduleTimes[1].enabled = true;
    blynkLog("Schedule updated: Slot 2 -> " + s);
  }
}

// V6: system enable/disable
BLYNK_WRITE(V6) {
  int val = param.asInt();
  systemEnabled = (val == 1);
  if (systemEnabled) {
    blynkStatus("System ENABLED");
    blynkLog("System enabled from Blynk");
  } else {
    blynkStatus("System DISABLED");
    blynkLog("System disabled from Blynk");
  }
}

// Connected event
BLYNK_CONNECTED() {
  blynkLog("Connected to Blynk cloud");
  blynkStatus("System online");
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(vibPin, INPUT);
  pinMode(irPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(redLedPin, OUTPUT);
  pinMode(greenLedPin, OUTPUT);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcdShowStatus("Connecting WiFi", "Please wait...", 2000);
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  if (!rtc.begin()) {
    lcdShowStatus("RTC error", "Check wiring", 2500);
    beep(3, 250);
  }

  for (int i = 0; i < (int)(sizeof(servoPins) / sizeof(servoPins[0])); i++) {
    servos[i].attach(servoPins[i]);
    servos[i].write(SERVO_HOME);
    delay(200);
  }

  lcdShowStatus("Pill Dispenser", "Blynk Ready", 2000);
  setLed(false, false);
  blynkLog("System initialized");
}

// ---------------- LOOP ----------------
void loop() {
  Blynk.run();

  DateTime now = rtc.now();

  if (!systemEnabled) {
    lcdShowStatus("System Disabled", "Use Blynk App", 2000);
    delay(500);
    return;
  }

  // Idle screen: real-time and greeting
  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "Time %02d:%02d:%02d", now.hour(), now.minute(), now.second());
  snprintf(line2, sizeof(line2), "%s", greetingForHour(now.hour()));
  lcdShowStatus(line1, line2, 1000);

  // Auto scheduled dispensing
  for (int i = 0; i < SCHEDULE_COUNT; i++) {
    if (scheduleTimes[i].enabled &&
        now.hour()   == scheduleTimes[i].hour &&
        now.minute() == scheduleTimes[i].minute) {
      if (!scheduleTriggered[i]) {
        char msg[17];
        snprintf(msg, sizeof(msg), "Slot %d is ready", scheduleTimes[i].slot + 1);
        lcdShowStatus(msg, "Auto dispense", 2000);
        blynkLog(String("Auto schedule triggered for slot ") + (scheduleTimes[i].slot + 1));
        dispenseSlot(scheduleTimes[i].slot);
        scheduleTriggered[i] = true;
      }
    } else {
      scheduleTriggered[i] = false;
    }
  }

  delay(500);
}