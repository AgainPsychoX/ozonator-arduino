// Program dla kontrolera ozonatora z możliwością ustawienia objętości
// pomieszczenia oraz dawki lub czasu (automatyczne przeliczanie wartości).
// Uwaga: Autor kodu nie ponosi żadnej odpowiedzialności za działanie programu.

/*****************************************************************************\

MIT License

Copyright (c) 2021 Patryk Ludwikowski <patryk.ludwikowski.7@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

\*****************************************************************************/

// Przy pierwszym uruchomieniu należy przytrzymać przycisk do góry (BUTTON_UP),
// aby wyczyścić ustawienia EEPROM - bez tego kroku mogą wczytać się śmieci.

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

//LiquidCrystal_I2C lcd(0x27, 20, 2); // W zależności od użytej biblioteki...
LiquidCrystal_I2C lcd(0x4E / 2, 2, 1, 0, 4, 5, 6, 7, 3, POSITIVE);

#define BUTTON_UP       12
#define BUTTON_ENTER    11
#define BUTTON_DOWN     10

#define BUTTON_PAUSE       BUTTON_ENTER

#define SOME_LED_PIN    9

#define RELAY_ENABLE_VALUE  HIGH // <-- Jakim stanem ma być sterowany przekaźnik? LOW - niski, HIGH - wysoki
#define RELAY_DISABLE_VALUE (RELAY_ENABLE_VALUE == LOW ? HIGH : LOW)

#define OZONATOR_PIN    6
const long ozonator_dose_per_second = round(3500.f / (60 * 60) * 1000); // Dawka w μg/s

#define FAN_PIN         5
#define FAN_DURATION_AFTER_WORKING 30000 // Czas dzialania wiatraka po wyłączeniu ozonatora w ms

#define BUZZER_PIN      3

#define EEPROM_ADDRESS 0

unsigned short menuIndex = 0;
bool changingValue = false;

float changeStep = 1;
#define CHANGE_STEP_FN(x) (x >= 100 ? 100 : (x * 1.09375f)) // 1+3/32
//#define CHANGE_STEP_FN(x) (x >= 10 ? 10 : (x * 1.09375f)) // 1+3/32
//#define CHANGE_STEP_FN(x) 1 // Odkomentować dla stałego kroku zmiany wartości
#define CHANGE_DELAY 100
#define CHANGE_STEP_MULTIPLIER_DOSE     10
#define CHANGE_STEP_MULTIPLIER_VOLUME   10
#define CHANGE_STEP_MULTIPLIER_TIME     1000 // 1000 ms == 1 s

// Change steps are constant for start timeout (by below, in seconds) and number of cycles (by 1)
#define CHANGE_STEP_START_TIMEOUT 10

bool paused = false;
unsigned long remaningTime = 0;
unsigned long lastUpdateTime = 0;

unsigned short currentCycle = 0;
#define CYCLE_DELAY (30UL * 60 * 1000) // Czas odczekiwany w ms po każdym cyklu
//#define FORCE_FAN_BETWEEN_CYCLES 1 // Odkomentować, jeśli wiatrak ma działać cały czas między cyklami.

typedef struct settings_t {
    unsigned long dose;     // μg/m^3 (m^3 == 1000 L)
    unsigned long volume;   // L
    unsigned long time;     // ms
    unsigned short cycles;
    unsigned short startTimeout; // seconds
} settings_t;
settings_t settings;

long calculateTime(long dose, long volume) {
    return floor((double) dose * volume / ozonator_dose_per_second);
}
long calculateDose(long time, long volume) {
    return round((double) ozonator_dose_per_second * time / volume);
}

byte skull_1[8] = { B00000, B00001, B00011, B00011, B00011, B00010, B01011, B11001 };
byte skull_2[8] = { B11111, B11111, B11111, B11111, B01110, B00100, B01110, B11011 };
byte skull_3[8] = { B00000, B10000, B11000, B11000, B11000, B01000, B11010, B10011 };
byte skull_4[8] = { B00100, B00000, B00000, B00001, B00001, B00100, B11000, B01000 };
byte skull_5[8] = { B11111, B10101, B00000, B01010, B11111, B11111, B00000, B00000 };
byte skull_6[8] = { B00100, B00000, B00000, B10000, B10000, B00100, B00011, B00010 };

void setup() {
    // Prepare pins while making sure the ozonator and fan are off
    {
        pinMode(OZONATOR_PIN, OUTPUT);
        digitalWrite(OZONATOR_PIN, RELAY_DISABLE_VALUE);

        pinMode(FAN_PIN, OUTPUT);
        digitalWrite(FAN_PIN, RELAY_DISABLE_VALUE);

#ifdef BUZZER_PIN
        pinMode(BUZZER_PIN, OUTPUT);
        digitalWrite(BUZZER_PIN, LOW);
#endif

        pinMode(SOME_LED_PIN, OUTPUT);
        digitalWrite(SOME_LED_PIN, LOW);

        pinMode(BUTTON_UP,  INPUT_PULLUP);
        pinMode(BUTTON_ENTER, INPUT_PULLUP);
        pinMode(BUTTON_DOWN, INPUT_PULLUP);
    }

    // Prepare LCD
    lcd.begin(20, 2);
    lcd.createChar(1, skull_1);
    lcd.createChar(2, skull_2);
    lcd.createChar(3, skull_3);
    lcd.createChar(4, skull_4);
    lcd.createChar(5, skull_5);
    lcd.createChar(6, skull_6);
    lcd.clear();

    // Start up screen
    {
        lcd.setCursor(1, 0);
        lcd.write(1);
        lcd.write(2);
        lcd.write(3);
        lcd.setCursor(1, 1);
        lcd.write(4);
        lcd.write(5);
        lcd.write(6);
        
        lcd.setCursor(20 - 4, 0);
        lcd.write(1);
        lcd.write(2);
        lcd.write(3);
        lcd.setCursor(20 - 4, 1);
        lcd.write(4);
        lcd.write(5);
        lcd.write(6);

        lcd.setCursor(6, 0);
        lcd.print("Ozonator");

        delay(3000);
        while (digitalRead(BUTTON_ENTER) == LOW);
    }
    
    // Prepare settings
    if (digitalRead(BUTTON_UP) == LOW) {
        // Using default settings
        settings.dose = 1000; // μg/m^3
        settings.volume = 3000; // L
        settings.time = calculateTime(settings.dose, settings.volume);
        settings.cycles = 1;
        settings.startTimeout = 0;
        
        // Overwriting EEPROM with default settings
        EEPROM.put(EEPROM_ADDRESS, settings);
    }
    else {
        // Load settings from EEPROM
        EEPROM.get(EEPROM_ADDRESS, settings);

        // Make sure time was calculated correctly
        settings.time = calculateTime(settings.dose, settings.volume);
    }

    // Blink indicator led on start
    digitalWrite(SOME_LED_PIN, HIGH);
    delay(250);
    digitalWrite(SOME_LED_PIN, LOW);
    delay(250);
    digitalWrite(SOME_LED_PIN, HIGH);
    delay(250);
    digitalWrite(SOME_LED_PIN, LOW);
}

void draw_menu() {
    lcd.setCursor(0, 0);
    switch (menuIndex) {
        case 0:
        case 1:
            //////////////////// 0 ////////////////////
            lcd.print(menuIndex == 0 ? '>' : ' ');
            lcd.print(F("Dawka "));
            lcd.setCursor(7, 0);
            if (menuIndex != 0 || millis() % 800 > 200 || !changingValue) {
                lcd.print(settings.dose / 1000);
                lcd.print('.');
#if (CHANGE_STEP_MULTIPLIER_DOSE % 100 == 0)
                unsigned short ug = (settings.dose / 100) % 10;
                lcd.print(ug);
#elif (CHANGE_STEP_MULTIPLIER_DOSE % 10 == 0)
                unsigned short ug = (settings.dose / 10) % 100;
                if (ug < 10) lcd.print('0');
                lcd.print(ug);
#else
                unsigned short ug = settings.dose % 1000;
                if (ug < 100) lcd.print('0');
                if (ug < 10) lcd.print('0');
                lcd.print(ug);
#endif                
                lcd.print(F(" mg/m3"));
            }
            lcd_print_spaces();
            //////////////////// 1 ////////////////////
            lcd.setCursor(0, 1);
            lcd.print(menuIndex == 1 ? '>' : ' ');
            lcd.print(F("Objetosc "));
            lcd.setCursor(10, 1);
            if (menuIndex != 1 || millis() % 800 > 200 || !changingValue) {
                lcd.print(settings.volume);
                lcd.print(F(" L"));
            }
            lcd_print_spaces();
            break;
        case 2:
        case 3:
            //////////////////// 2 ////////////////////
            lcd.print(menuIndex == 2 ? '>' : ' ');
            lcd.print(F(" Czas: "));
            lcd.setCursor(8, 0);
            if (menuIndex != 2 || millis() % 800 > 200 || !changingValue) {
                lcd.print(settings.time / 60000);
                //lcd.print(':');
                lcd.print(F("min "));
                unsigned short s = settings.time / 1000 % 60;
                //if (s < 10) lcd.print('0');
                lcd.print(s);
                lcd.print(F("sek"));
            }
            lcd_print_spaces();
            //////////////////// 3 ////////////////////
            lcd.setCursor(0, 1);
            lcd.print(menuIndex == 3 ? '>' : ' ');
            lcd.print(F(" Start?"));
            lcd_print_spaces();
            break;
        case 4:
        case 5:
            //////////////////// 4 ////////////////////
            lcd.print(menuIndex == 4 ? '>' : ' ');
            lcd.print(F("Opu. startu: "));
            if (menuIndex != 4 || millis() % 800 > 200 || !changingValue) {
                lcd.print(settings.startTimeout);
                lcd.print('s');
            }
            lcd_print_spaces();
            //////////////////// 5 ////////////////////
            lcd.setCursor(0, 1);
            lcd.print(menuIndex == 5 ? '>' : ' ');
            lcd.print(F("Liczba cykli: "));
            if (menuIndex != 5 || millis() % 800 > 200 || !changingValue) {
                lcd.print(settings.cycles);
            }
            lcd_print_spaces();
            break;
    }
}

// TODO: generating menu code using macros?

inline bool menu_loop() {
    draw_menu();
    
    if (digitalRead(BUTTON_UP) == LOW) {
        if (changingValue) {
            const unsigned int changeStepFloored = floor(changeStep);
            switch (menuIndex) {
                case 0: 
                    settings.dose += changeStepFloored * CHANGE_STEP_MULTIPLIER_DOSE;
                    settings.time = calculateTime(settings.dose, settings.volume);
                    break;
                case 1: 
                    settings.volume += changeStepFloored * CHANGE_STEP_MULTIPLIER_VOLUME;
                    settings.time = calculateTime(settings.dose, settings.volume);
                    break;
                case 2: 
                    settings.time += changeStepFloored * CHANGE_STEP_MULTIPLIER_TIME;
                    settings.dose = calculateDose(settings.time, settings.volume);
                    break;
                case 3:
                    // Start, nothing to change.
                    break;
                case 4:
                    settings.startTimeout += CHANGE_STEP_START_TIMEOUT;
                    delay(CHANGE_DELAY * 3);
                    break;
                case 5:
                    settings.cycles += 1;
                    delay(CHANGE_DELAY * 3);
                    break;
            }
            changeStep = CHANGE_STEP_FN(changeStep);
            delay(CHANGE_DELAY);
        }
        else {
            if (menuIndex > 0) {
                menuIndex -= 1;
            }
            else {
               menuIndex = 5;
            }
            delay(333);
        }
    }
    else if (digitalRead(BUTTON_DOWN) == LOW) {
        if (changingValue) {
            const unsigned int changeStepFloored = floor(changeStep);
            switch (menuIndex) {
                case 0: 
                    if (settings.dose > changeStepFloored * CHANGE_STEP_MULTIPLIER_DOSE) {
                        settings.dose -= changeStepFloored * CHANGE_STEP_MULTIPLIER_DOSE;
                    }
                    settings.time = calculateTime(settings.dose, settings.volume);
                    break;
                case 1: 
                    if (settings.volume > changeStepFloored * CHANGE_STEP_MULTIPLIER_VOLUME) {
                        settings.volume -= changeStepFloored * CHANGE_STEP_MULTIPLIER_VOLUME;
                    }
                    settings.time = calculateTime(settings.dose, settings.volume);
                    break;
                case 2: 
                    if (settings.time > changeStepFloored * CHANGE_STEP_MULTIPLIER_TIME) {
                        settings.time -= changeStepFloored * CHANGE_STEP_MULTIPLIER_TIME;
                    }
                    settings.dose = calculateDose(settings.time, settings.volume);
                    break;
                case 3:
                    // Start, nothing to change.
                    break;
                case 4:
                    if (settings.startTimeout >= CHANGE_STEP_START_TIMEOUT) {
                        settings.startTimeout -= CHANGE_STEP_START_TIMEOUT;
                    }
                    delay(CHANGE_DELAY * 3);
                    break;
                case 5: 
                    if (settings.cycles > 1) {
                        settings.cycles -= 1;
                    }
                    delay(CHANGE_DELAY * 3);
                    break;
            }
            changeStep = CHANGE_STEP_FN(changeStep);
            delay(CHANGE_DELAY);
        }
        else {
            if (menuIndex < 5) {
                menuIndex += 1;
            }
            else {
                menuIndex = 0;
            }
            delay(333);
        }
    }
    else if (digitalRead(BUTTON_ENTER) == LOW) {
        switch (menuIndex) {
            case 3: {
                // Save settings (even if start not accepted by holding button)
                EEPROM.put(EEPROM_ADDRESS, settings);
                
                // Detect start accepting (by holding button)
                bool accepted = true;
                lcd.setCursor(10, 1);
                for (unsigned short i = 0; i < 5; i++) {
                    delay(250);
                    lcd.print('.');
                    lcd.print(' ');
                    if (digitalRead(BUTTON_ENTER) == HIGH) {
                        accepted = false;
                        break;
                    }
                }
                if (accepted) {
                    // Wait for button release
                    while (digitalRead(BUTTON_ENTER) == LOW) {
                        delay(100);
                    }

                    // Wait through start timeout, or for force start (both up and down buttons pressed)
                    if (settings.startTimeout > 0) {
                        remaningTime = settings.startTimeout * 1000;
                        lastUpdateTime = millis();
                        while (timeoutBeforeWorking_loop());
                    }

                    for (currentCycle = 1; currentCycle <= settings.cycles; currentCycle += 1) {
#ifdef BUZZER_PIN
                        // Make buzzer signal on start
                        tone(BUZZER_PIN, 1000, 1000);
#endif

                        // Start the ozonator and fan
                        digitalWrite(OZONATOR_PIN, RELAY_ENABLE_VALUE);
                        digitalWrite(FAN_PIN, RELAY_ENABLE_VALUE);

                        // Working and counting time
                        remaningTime = settings.time;
                        lastUpdateTime = millis();
                        while (working_loop());

                        // Turn off the ozonator
                        digitalWrite(OZONATOR_PIN, RELAY_DISABLE_VALUE);

                        // Make sure the led indicator is off after working
                        digitalWrite(SOME_LED_PIN, LOW);

                        // For all, but last cycle...
                        if (currentCycle < settings.cycles) {
#if FORCE_FAN_BETWEEN_CYCLES
                            // Wait through cycle delay
                            remaningTime = CYCLE_DELAY;
                            lastUpdateTime = millis();
                            while (delayAfterCycle_loop());
#else
                            // Wait through fan duration after working
                            remaningTime = FAN_DURATION_AFTER_WORKING;
                            lastUpdateTime = millis();
                            while (fanAfterCycle_loop());

                            // Turn off the fan
                            digitalWrite(FAN_PIN, RELAY_DISABLE_VALUE);

                            // Wait through cycle delay
                            remaningTime = CYCLE_DELAY - FAN_DURATION_AFTER_WORKING;
                            lastUpdateTime = millis();
                            while (delayAfterCycle_loop());
#endif
                        }
                    }
                    
                    // Show work done screen
                    lcd.clear();
                    lcd.setCursor(0, 0);
                    lcd.print(F(" Koniec, przewietrz "));
                    lcd.setCursor(0, 1);
                    lcd.print(F("   pomieszczenie!!  "));

                    // Buzzer alarm
#ifdef BUZZER_PIN
                    for (short i = 0; i < 3; i++) {
                        tone(BUZZER_PIN, 1000);
                        delay(1000);
                        noTone(BUZZER_PIN);
                        delay(1000);
                    }
#else
                    delay(6000)
#endif

                    // Wait for button press or timeout
                    unsigned long ms = 0;
                    while (digitalRead(BUTTON_ENTER) == HIGH) {
                        delay(100);
                        ms += 100;
                        if (ms >= FAN_DURATION_AFTER_WORKING) {
                            break;
                        }
                    }

                    // Stop fan (and make sure ozonator is off as well)
                    digitalWrite(OZONATOR_PIN, RELAY_DISABLE_VALUE);
                    digitalWrite(FAN_PIN, RELAY_DISABLE_VALUE);

                    // Wait for button press anyway
                    if (ms >= FAN_DURATION_AFTER_WORKING) {
                        while (digitalRead(BUTTON_ENTER) == HIGH) {
                            delay(100);
                        }
                    }

                    // Wait for button release
                    while (digitalRead(BUTTON_ENTER) == LOW) {
                        delay(100);
                    }

                    // Jump to beginning of menu
                    menuIndex = 0;

                    // Calculate remaning dose, if canceled
                    if (remaningTime > 1000) {                        
                        settings.dose = calculateDose(settings.time, settings.volume);
                    }
                }
                break;
            }
            default: {
                changingValue = !changingValue;
                changeStep = 1;
                delay(500);
                break;
            }
        }
    }
    else {
        changeStep = 1;
        delay(50);
    }
    
    return true;
}

bool working_loop() {
    lcd.setCursor(0, 0);
    lcd.print(F("Uwaga!! Odkarzanie!!"));
    lcd.setCursor(0, 1);
    lcd.print(F("Czas do konca "));
    lcd.print(remaningTime / 1000 / 60);
    lcd.print(':');
    unsigned short sec = remaningTime / 1000 % 60;
    if (sec < 10) lcd.print('0');
    lcd.print(sec);
    lcd.print(F("          "));
    
    unsigned long now = millis();
    unsigned long delta = now - lastUpdateTime;
    lastUpdateTime = now;
    if (remaningTime < delta) {
        remaningTime = 0;
        return false;
    }
    remaningTime -= delta;

    // Blink while working
    if (now % 500 > 250) {
        digitalWrite(SOME_LED_PIN, HIGH);
    }
    else {
        digitalWrite(SOME_LED_PIN, LOW);
    }

    // Pausing
    if (digitalRead(BUTTON_PAUSE) == LOW) {
        lcd.setCursor(0, 0);
        lcd.print(F(" Pauza w odkarzaniu "));

        // Stopping the ozonator
        digitalWrite(OZONATOR_PIN, RELAY_DISABLE_VALUE);

        // Wait for button release
        while (digitalRead(BUTTON_PAUSE) == LOW);

        // Wait for button to unpause
        while (digitalRead(BUTTON_PAUSE) == HIGH);

        // Unpausing
        digitalWrite(OZONATOR_PIN, RELAY_ENABLE_VALUE);
        lastUpdateTime = millis();
    }

    // Force start if both up and down buttons are pressed
    if (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW) {
        // Wait for release
        while (digitalRead(BUTTON_UP) == LOW || digitalRead(BUTTON_DOWN) == LOW) {
            delay(50);
        }

        remaningTime = 0;
    }

    delay(100);
    return true;
}

bool timeout_loop() {
    // Count down the timeout
    unsigned long now = millis();
    unsigned long delta = now - lastUpdateTime;
    lastUpdateTime = now;
    if (remaningTime < delta) {
        remaningTime = 0;
        return false;
    }
    remaningTime -= delta;
    
    // Force start if both up and down buttons are pressed
    if (digitalRead(BUTTON_UP) == LOW && digitalRead(BUTTON_DOWN) == LOW) {
        // Wait for release
        while (digitalRead(BUTTON_UP) == LOW || digitalRead(BUTTON_DOWN) == LOW) {
            delay(50);
        }

        remaningTime = 0;
    }

    delay(100);
    return true;
}

bool timeoutBeforeWorking_loop() {
    lcd.setCursor(0, 0);
    lcd.print(F("   Czas do startu   "));
    lcd.setCursor(0, 1);
    lcd.print(F("  odkazania: "));
    lcd.print(remaningTime / 1000 / 60);
    lcd.print(':');
    unsigned short sec = remaningTime / 1000 % 60;
    if (sec < 10) lcd.print('0');
    lcd.print(sec);
    lcd.print(F("          "));
    
    return timeout_loop();
}

bool fanAfterCycle_loop() {
    const unsigned long remaningTimeWithDelay = remaningTime + (CYCLE_DELAY - FAN_DURATION_AFTER_WORKING);
    lcd.setCursor(0, 0);
    lcd.print(F(" Koniec "));
    lcd.print(currentCycle);
    lcd.print(F(" cyklu      "));
    lcd.setCursor(0, 1);
    lcd.print(F(" Kolejny za: "));
    lcd.print(remaningTimeWithDelay / 1000 / 60);
    lcd.print(':');
    unsigned short sec = remaningTimeWithDelay / 1000 % 60;
    if (sec < 10) lcd.print('0');
    lcd.print(sec);
    lcd.print(F("          "));

    return timeout_loop();
}

bool delayAfterCycle_loop() {
    lcd.setCursor(0, 0);
    lcd.print(F(" Koniec "));
    lcd.print(currentCycle);
    lcd.print(F(" cyklu      "));
    lcd.setCursor(0, 1);
    lcd.print(F(" Kolejny za: "));
    lcd.print(remaningTime / 1000 / 60);
    lcd.print(':');
    unsigned short sec = remaningTime / 1000 % 60;
    if (sec < 10) lcd.print('0');
    lcd.print(sec);
    lcd.print(F("          "));
    
    return timeout_loop();
}

void loop() {
    menu_loop();
}
