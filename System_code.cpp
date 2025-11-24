#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <RTClib.h>

// --- Hardware Pins ---
const int servoPins[]   = {13, 12};    // Edit/add as needed for more slots
const int vibPin        = 33;
const int irPin         = 32;
const int buzzerPin     = 19;
const int redLedPin     = 26;
const int greenLedPin   = 18;

// --- LCD, RTC, Servo Setup ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
Servo servos[sizeof(servoPins)/sizeof(servoPins[0])];

// --- Servo Positions ---
const int SERVO_HOME      = 180;
const int SERVO_DISPENSE  = 0;
const int SERVO_RETURN    = 180;

// --- Detection & Retry ---
const unsigned long VIBRATION_TIMEOUT_MS = 2000;
const unsigned long IR_CONFIRM_TIMEOUT_MS = 60000;  // 1 minute for pill pickup
const int MAX_RETRIES = 6;

// --- Message display times ---
const unsigned long MESSAGE_DISPLAY_MS = 2000; // Each message shows ≥ 2s

// --- Schedule ---
// Edit/add as needed for more slots
struct DispenseSchedule { uint8_t hour; uint8_t minute; uint8_t slot; };
DispenseSchedule scheduleTimes[] = {
    {13, 49, 0},
    {13, 50, 1}
};
const int SCHEDULE_COUNT = sizeof(scheduleTimes)/sizeof(DispenseSchedule);
bool scheduleTriggered[SCHEDULE_COUNT] = {false};

// --- LCD Double Buffer ---
char lastLine1[17] = "";
char lastLine2[17] = "";
void lcdShowStatus(const char *line1, const char *line2, int showMillis = MESSAGE_DISPLAY_MS) {
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
    delay(showMillis);
}

// --- Beep and LED helpers ---
void beep(int times = 1, int ms = 120) {
    for (int i = 0; i < times; i++) {
        digitalWrite(buzzerPin, HIGH);
        delay(ms);
        digitalWrite(buzzerPin, LOW);
        delay(70);
    }
}
void setLed(bool greenOn, bool redOn) {
    digitalWrite(greenLedPin, greenOn ? HIGH : LOW);
    digitalWrite(redLedPin, redOn ? HIGH : LOW);
}

// --- Wait for vibration (pill dropped) ---
bool waitForVibration(unsigned long timeoutMs) {
    unsigned long start = millis();
    int confirmed = 0;
    while (millis() - start < timeoutMs) {
        if (digitalRead(vibPin) == HIGH) {
            confirmed++;
            if (confirmed >= 3) return true;
        } else {
            confirmed = 0;
        }
        delay(40);
    }
    return false;
}

// --- Wait for pill presence in outlet (IR sensor) ---
bool waitForIR(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (digitalRead(irPin) == LOW) return true;  // Outlet opened/pill detected
        delay(30);
    }
    return false;
}

// --- Wait for pill pickup (IR goes HIGH = pill taken) ---
bool waitForPillPickup(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (digitalRead(irPin) == HIGH) return true; // Pill removed
        delay(100);
    }
    return false;
}

// --- Greeting based on time ---
const char* greetingForHour(int hour) {
    if (hour >= 5 && hour < 12)  return "Good Morning!";
    if (hour >= 12 && hour < 17) return "Good Afternoon!";
    if (hour >= 17 && hour < 21) return "Good Evening!";
    return "Good Night!";
}

// --- Main dispensing logic with vibration + IR confirmation and retry ---
void dispenseSlot(int slot) {
    char buf1[17], buf2[17];
    snprintf(buf1, sizeof(buf1), "Slot %d Ready", slot + 1);
    lcdShowStatus(buf1, "Dispensing...", MESSAGE_DISPLAY_MS);

    setLed(false, false);
    beep(1, 180);   // Longer beep for notice

    int attempt = 0;
    bool dispensed = false;

    // Try dispensing until vibration is confirmed
    while (attempt < MAX_RETRIES && !dispensed) {
        attempt++;

        // Servo move: pick, dispense, return
        servos[slot].write(SERVO_DISPENSE);
        delay(1000);
        servos[slot].write(SERVO_RETURN);
        delay(1000);
        servos[slot].write(SERVO_HOME);
        delay(1000);

        // Confirm vibration
        bool vib = waitForVibration(VIBRATION_TIMEOUT_MS);

        if (vib) {
            dispensed = true;
            lcdShowStatus(buf1, "Pill Dispensed!", MESSAGE_DISPLAY_MS);
            setLed(true, false);  // green led

            beep(1, 220);

            // Wait for IR confirmation of pill pickup within allowed time
            lcdShowStatus("Take your pill!", "Waiting removal", MESSAGE_DISPLAY_MS);

            bool isPillTaken = waitForIR(IR_CONFIRM_TIMEOUT_MS);

            if (isPillTaken) {
                lcdShowStatus("Pill taken!", "Thank you!", MESSAGE_DISPLAY_MS);
                beep(2, 180);
                setLed(false, false);
            } else {
                lcdShowStatus("Alert!", "Pill not taken!", MESSAGE_DISPLAY_MS);
                beep(3, 200);
                setLed(false, true);
                // Add caretaker notification code here!
            }
        } else {
            snprintf(buf2, sizeof(buf2), "Retry %d/%d", attempt, MAX_RETRIES);
            lcdShowStatus("Dispense Fail", buf2, MESSAGE_DISPLAY_MS);
            setLed(false, true);
            beep(2, 120);
        }
    }

    if (!dispensed) {
        lcdShowStatus("CRITICAL ALERT!", "Dispense Failed!", MESSAGE_DISPLAY_MS);
        setLed(false, true);
        beep(5, 200);
        // Caretaker alert logic: send notification, etc.
    }

    servos[slot].write(SERVO_HOME);
    setLed(false, false);
    delay(1000);
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    delay(150);

    pinMode(vibPin, INPUT);
    pinMode(irPin, INPUT);
    pinMode(buzzerPin, OUTPUT);
    pinMode(redLedPin, OUTPUT);
    pinMode(greenLedPin, OUTPUT);

    Wire.begin(21, 22);

    lcd.init(); lcd.backlight(); lcd.clear();

    if (!rtc.begin()) {
        lcdShowStatus("RTC error", "Check wiring", MESSAGE_DISPLAY_MS);
        beep(3, 180);
        delay(2000);
    }

    for (int i = 0; i < sizeof(servoPins)/sizeof(servoPins[0]); i++) {
        servos[i].attach(servoPins[i]);
        servos[i].write(SERVO_HOME);
        delay(150);
    }

    lcdShowStatus("Pill Dispenser", "Ready!", MESSAGE_DISPLAY_MS);
    setLed(false, false);
    delay(1000);
}

// --- LOOP: advanced logic for real-time, greetings, and robust scheduling ---
void loop() {
    DateTime now = rtc.now();

    // Display greeting and current time when idle (always visible, changing with time)
    char line1[17], line2[17];
    snprintf(line1, sizeof(line1), "Time %02d:%02d:%02d", now.hour(), now.minute(), now.second());
    snprintf(line2, sizeof(line2), "%s", greetingForHour(now.hour()));
    lcdShowStatus(line1, line2, MESSAGE_DISPLAY_MS);

    // Scheduled dispensing for all slots
    for (int i = 0; i < SCHEDULE_COUNT; i++) {
        if (now.hour() == scheduleTimes[i].hour && now.minute() == scheduleTimes[i].minute) {
            if (!scheduleTriggered[i]) {
                char msg[17];
                snprintf(msg, sizeof(msg), "Slot %d is ready", scheduleTimes[i].slot + 1);
                lcdShowStatus(msg, "", MESSAGE_DISPLAY_MS);
                // Dispense pill for that slot
                dispenseSlot(scheduleTimes[i].slot);
                scheduleTriggered[i] = true;
            }
        } else {
            // Reset for next scheduled trigger time
            scheduleTriggered[i] = false;
        }
    }

    delay(1500);   // LCD always refreshed (≥1s for visibility)
}
