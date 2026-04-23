/*************************************************************
 *  MycoControl Groupe 2 — ESP32-S3 VERSION CORRIGÉE
 * ═══════════════════════════════════════════════════════════
 *
 *  CORRECTIONS APPLIQUÉES :
 *  1. PIN_MQ7 était GPIO 01 (= TX Serial) → corrigé GPIO 36
 *  2. ledcWrite(canal, val) → ledcWrite(PIN, val)  [core 3.x]
 *  3. ledcAttach() maintenant AVANT analogRead/autres
 *  4. buzzerBip() sans delay() bloquant → utilise millis()
 *  5. WiFi : mode WIFI_STA seulement (pas AP+STA qui cause WDT)
 *  6. Timeout WiFi propre sans WDT reset
 *  7. Watchdog nourri pendant les longues attentes
 *  8. Firebase : database.url() après initializeApp correct
 *  9. app.loop() appelé dans loop() (obligatoire FirebaseClient)
 *
 *  BROCHAGE ESP32-S3 :
 *  ┌─────────────────────────────────────────────────────┐
 *  │  DHT22 DATA       → GPIO  4                        │
 *  │  MQ7  AO (analog) → GPIO 36  (ADC — entrée only)  │
 *  │  L298N ENA (PWM)  → GPIO 25                        │
 *  │  L298N IN1        → GPIO 26                        │
 *  │  L298N IN2        → GPIO 27                        │
 *  │  L298N ENB (PWM)  → GPIO 14                        │
 *  │  L298N IN3        → GPIO 12                        │
 *  │  L298N IN4        → GPIO 13                        │
 *  │  Servo signal     → GPIO 33                        │
 *  │  Relais vanne     → GPIO 17                        │
 *  │  LED nocturne     → GPIO 19                        │
 *  │  Buzzer (+)       → GPIO 15                        │
 *  │  LCD SDA          → GPIO 21  (I²C défaut)          │
 *  │  LCD SCL          → GPIO 22  (I²C défaut)          │
 *  └─────────────────────────────────────────────────────┘
 *
 *  HOTSPOT TÉLÉPHONE :
 *  - Activez le partage de connexion sur votre téléphone
 *  - SSID et mot de passe ci-dessous DOIVENT correspondre
 *    exactement à ceux de votre hotspot (sensible à la casse)
 *
 *  BIBLIOTHÈQUES (Arduino IDE Library Manager) :
 *  - FirebaseClient by Mobizt
 *  - DHT sensor library by Adafruit
 *  - LiquidCrystal I2C by Frank de Brabander
 *  - ESP32Servo by Kevin Harrington
 *************************************************************/

#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>       // ← Pour nourrir le watchdog
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include "DHT.h"
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ════════════════════════════════════════════════
//  ▶▶ MODIFIEZ ICI — SSID ET MOT DE PASSE HOTSPOT
// ════════════════════════════════════════════════
const char* WIFI_SSID = "webchampignon";  // ← Nom exact du hotspot téléphone
const char* WIFI_PASS = "12345678";       // ← Mot de passe exact

// ════════════════════════════════════════════════
//  FIREBASE
// ════════════════════════════════════════════════
#define DATABASE_URL "https://webchampignon-default-rtdb.firebaseio.com"

// NTP — Madagascar UTC+3
#define NTP_SERVER     "pool.ntp.org"
#define GMT_OFFSET_S   10800
#define DAYLIGHT_S     0

// ════════════════════════════════════════════════
//  GPIO — TOUS VÉRIFIÉS POUR ESP32-S3
// ════════════════════════════════════════════════
#define PIN_DHT       4    // DHT22 DATA
#define DHTTYPE       DHT22

// ⚠️ MQ7 : GPIO 36 = ADC entrée seulement (CORRECT)
// GPIO 01 était TX du Serial → crash garanti !
#define PIN_MQ7      36    // MQ7 sortie analogique

// L298N Ventilateur A
#define PIN_ENA      25    // PWM ENA
#define PIN_IN1      26
#define PIN_IN2      27

// L298N Ventilateur B
#define PIN_ENB      14    // PWM ENB
#define PIN_IN3      12
#define PIN_IN4      13

#define PIN_SERVO    33    // Servomoteur
#define PIN_VANNE    48    // Relais électrovanne
#define PIN_LUMIERED 19    // LED lumière nocturne
#define PIN_BUZZER   47    // Buzzer actif

// ════════════════════════════════════════════════
//  SEUILS MQ7
// ════════════════════════════════════════════════
#define MQ7_THRESHOLD 2000   // Valeur ADC 0–4095 → alarme si dépassé

// ════════════════════════════════════════════════
//  PARAMÈTRES PAR PHASE
// ════════════════════════════════════════════════
struct PhaseConfig {
    float targetT;
    float targetH;
    int   servoAngle;
    int   fanSpeed;      // 0–255
};

// Index 0 = non utilisé (phases 1–4)
const PhaseConfig PHASES[5] = {
    {0,    0,    0,   0  },  // 0 — vide
    {26.5, 65.0, 0,   150},  // 1 — Préparation
    {25.0, 87.5, 45,  180},  // 2 — Incubation
    {22.0, 92.0, 90,  220},  // 3 — Fructification
    {6.0,  80.0, 135, 100},  // 4 — Récolte
};

// ════════════════════════════════════════════════
//  OBJETS
// ════════════════════════════════════════════════
DHT               dht(PIN_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo             myServo;

WiFiClientSecure  ssl_client;
AsyncClientClass  fbClient(ssl_client);
FirebaseApp       fbApp;
RealtimeDatabase  database;
AsyncResult       fbResult;
NoAuth            no_auth;

// ════════════════════════════════════════════════
//  ÉTAT GLOBAL
// ════════════════════════════════════════════════
int   currentPhase = 1;
bool  alarmTemp    = false;
bool  alarmCO2     = false;
bool  nightLightOn = false;
bool  ventiloAuto  = true;
bool  vanneAuto    = true;
int   fanSpeedA    = 0;
int   fanSpeedB    = 0;
int   servoAngle   = 0;
int   co2Raw       = 0;
float lastT        = 0.0;
float lastH        = 0.0;
bool  fbConnected  = false;

// ── Buzzer non-bloquant ───────────────────────────────────
struct BuzzerState {
    bool     active   = false;
    int      total    = 0;
    int      done     = 0;
    bool     bipOn    = false;
    unsigned long t0  = 0;
    int      onMs     = 200;
    int      offMs    = 100;
} buz;

// ── Timers ────────────────────────────────────────────────
unsigned long tmrSensor   = 0;
unsigned long tmrFbWrite  = 0;
unsigned long tmrFbRead   = 0;
unsigned long tmrSchedule = 0;
unsigned long tmrPrint    = 0;
unsigned long tmrWifi     = 0;

#define INTERVAL_SENSOR    2000
#define INTERVAL_FB_WRITE  3000
#define INTERVAL_FB_READ   4000
#define INTERVAL_SCHEDULE  5000
#define INTERVAL_PRINT     4000
#define INTERVAL_WIFI_CHK  15000

// ════════════════════════════════════════════════
//  BUZZER NON BLOQUANT
//  N'utilise PAS delay() → ne bloque plus le WDT
// ════════════════════════════════════════════════
void buzzerStart(int n, int onMs = 250, int offMs = 120) {
    buz.active = true;
    buz.total  = n;
    buz.done   = 0;
    buz.bipOn  = false;
    buz.onMs   = onMs;
    buz.offMs  = offMs;
    buz.t0     = millis();
    digitalWrite(PIN_BUZZER, HIGH);
    buz.bipOn = true;
}

void buzzerTick() {
    if (!buz.active) return;
    unsigned long now = millis();
    if (buz.bipOn && (now - buz.t0 >= (unsigned long)buz.onMs)) {
        digitalWrite(PIN_BUZZER, LOW);
        buz.bipOn = false;
        buz.done++;
        buz.t0 = now;
        if (buz.done >= buz.total) { buz.active = false; }
    } else if (!buz.bipOn && buz.done < buz.total && (now - buz.t0 >= (unsigned long)buz.offMs)) {
        digitalWrite(PIN_BUZZER, HIGH);
        buz.bipOn = true;
        buz.t0 = now;
    }
}

// ════════════════════════════════════════════════
//  L298N — Régler vitesse (ESP32 core 3.x)
//  ledcWrite(PIN, duty) — plus de canal numérique
// ════════════════════════════════════════════════
void setFans(int speedA, int speedB) {
    fanSpeedA = constrain(speedA, 0, 255);
    fanSpeedB = constrain(speedB, 0, 255);

    // Ventilateur A
    if (fanSpeedA > 0) {
        digitalWrite(PIN_IN1, HIGH);
        digitalWrite(PIN_IN2, LOW);
        ledcWrite(PIN_ENA, fanSpeedA);   // ← PIN, pas canal
    } else {
        digitalWrite(PIN_IN1, LOW);
        digitalWrite(PIN_IN2, LOW);
        ledcWrite(PIN_ENA, 0);
    }

    // Ventilateur B
    if (fanSpeedB > 0) {
        digitalWrite(PIN_IN3, HIGH);
        digitalWrite(PIN_IN4, LOW);
        ledcWrite(PIN_ENB, fanSpeedB);   // ← PIN, pas canal
    } else {
        digitalWrite(PIN_IN3, LOW);
        digitalWrite(PIN_IN4, LOW);
        ledcWrite(PIN_ENB, 0);
    }
}

void setServo(int angle) {
    angle = constrain(angle, 0, 180);
    servoAngle = angle;
    myServo.write(angle);
}

void setNightLight(bool on) {
    nightLightOn = on;
    digitalWrite(PIN_LUMIERED, on ? HIGH : LOW);
}

// ════════════════════════════════════════════════
//  MQ7 — Lecture ADC (GPIO 36, entrée seulement)
// ════════════════════════════════════════════════
int readCO2() {
    long sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += analogRead(PIN_MQ7);
        delay(5);     // court délai acceptable (5ms × 5 = 25ms total)
    }
    return (int)(sum / 5);
}

// ════════════════════════════════════════════════
//  AUTOMATION PAR PHASE
// ════════════════════════════════════════════════
void controlAutomation(float t, float h) {
    if (currentPhase < 1 || currentPhase > 4) return;
    const PhaseConfig& cfg = PHASES[currentPhase];

    // Ventilateurs (si mode auto)
    if (ventiloAuto) {
        if (t > cfg.targetT) {
            setFans(cfg.fanSpeed, cfg.fanSpeed);
        } else {
            setFans(0, 0);
        }
    }

    // Électrovanne (si mode auto)
    if (vanneAuto) {
        digitalWrite(PIN_VANNE, (h < cfg.targetH) ? HIGH : LOW);
    }

    // Servo automatique
    if (fanSpeedA > 0) {
        setServo(cfg.servoAngle);
    } else {
        setServo(0);
    }

    // ── Alarme température ──────────────────────────────────
    bool newAlarmT = (t > cfg.targetT + 2.0f);
    if (newAlarmT && !alarmTemp) {
        alarmTemp = true;
        Serial.printf("[ALARME TEMP] %.1f°C > %.1f°C !\n", t, cfg.targetT + 2.0f);
        buzzerStart(3, 400, 150);
    } else if (!newAlarmT && alarmTemp) {
        alarmTemp = false;
        Serial.println("[OK] Température revenue à la normale");
        buzzerStart(1, 80, 0);
    }

    // ── Alarme CO2 MQ7 ──────────────────────────────────────
    bool newAlarmC = (co2Raw > MQ7_THRESHOLD);
    if (newAlarmC && !alarmCO2) {
        alarmCO2 = true;
        Serial.printf("[ALARME CO2] ADC=%d > %d !\n", co2Raw, MQ7_THRESHOLD);
        buzzerStart(5, 200, 80);
        setFans(255, 255);    // ventilation forcée max
    } else if (!newAlarmC && alarmCO2) {
        alarmCO2 = false;
        Serial.println("[OK] CO2 revenu à la normale");
    }
}

// ════════════════════════════════════════════════
//  LUMIÈRE NOCTURNE — 17h30 → 06h00 (NTP)
// ════════════════════════════════════════════════
void controlNightSchedule() {
    struct tm ti = {};
    time_t now   = time(nullptr);
    if (now < 100000UL) return;   // NTP pas encore synchronisé

    localtime_r(&now, &ti);
    int nowMin = ti.tm_hour * 60 + ti.tm_min;
    int onMin  = 17 * 60 + 30;   // 17:30
    int offMin =  6 * 60;         // 06:00

    bool shouldBeOn = (nowMin >= onMin || nowMin < offMin);

    if (shouldBeOn != nightLightOn) {
        setNightLight(shouldBeOn);
        Serial.printf("[NUIT] Lumière → %s (%02d:%02d)\n",
            shouldBeOn ? "ON" : "OFF", ti.tm_hour, ti.tm_min);
    }
}

// ════════════════════════════════════════════════
//  ÉCRITURE FIREBASE — ESP32 → Cloud
// ════════════════════════════════════════════════
void writeToFirebase() {
    if (!fbConnected) return;

    // /sensor/ — chemins lus par le site web
    database.set(fbClient, DATABASE_URL "/sensor/temperature", String(lastT, 1));
    database.set(fbClient, DATABASE_URL "/sensor/humidity",    String(lastH, 1));

    // /status/ — tous les états
    database.set(fbClient, DATABASE_URL "/status/temp",       String(lastT, 1));
    database.set(fbClient, DATABASE_URL "/status/hum",        String(lastH, 1));
    database.set(fbClient, DATABASE_URL "/status/co2",        String(co2Raw));
    database.set(fbClient, DATABASE_URL "/status/ventilo",    String(fanSpeedA > 0 ? 1 : 0));
    database.set(fbClient, DATABASE_URL "/status/vanne",      String(digitalRead(PIN_VANNE)));
    database.set(fbClient, DATABASE_URL "/status/nightlight", String(nightLightOn ? 1 : 0));
    database.set(fbClient, DATABASE_URL "/status/servo",      String(servoAngle));
    database.set(fbClient, DATABASE_URL "/status/fanSpeedA",  String(fanSpeedA));
    database.set(fbClient, DATABASE_URL "/status/fanSpeedB",  String(fanSpeedB));
    database.set(fbClient, DATABASE_URL "/status/targetT",    String(PHASES[currentPhase].targetT, 1));
    database.set(fbClient, DATABASE_URL "/status/targetH",    String(PHASES[currentPhase].targetH, 1));

    // /alarms/
    database.set(fbClient, DATABASE_URL "/alarms/temp",    String(alarmTemp ? 1 : 0));
    database.set(fbClient, DATABASE_URL "/alarms/co2",     String(alarmCO2  ? 1 : 0));

    String msg = "Systeme OK";
    if (alarmTemp && alarmCO2) msg = "ALARME TEMP + CO2 !";
    else if (alarmTemp)        msg = "ALARME Temp: " + String(lastT, 1) + "C";
    else if (alarmCO2)         msg = "ALARME CO2: ADC=" + String(co2Raw);
    database.set(fbClient, DATABASE_URL "/alarms/message", msg);
}

// ════════════════════════════════════════════════
//  LECTURE FIREBASE — Cloud → ESP32 (commandes web)
// ════════════════════════════════════════════════
void readFromFirebase() {
    if (!fbConnected) return;

    AsyncResult res;

    // Phase
    database.get(fbClient, DATABASE_URL "/config/currentPhase", res);
    if (res.available()) {
        int ph = res.to<RealtimeDatabaseResult>().to<int>();
        if (ph >= 1 && ph <= 4 && ph != currentPhase) {
            currentPhase = ph;
            Serial.printf("[FB] Phase → %d\n", currentPhase);
        }
    }

    // Ventilateur (0=auto, 1=OFF forcé, 2=ON forcé)
    database.get(fbClient, DATABASE_URL "/controls/ventilo", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if      (v == 0) { ventiloAuto = true; }
        else if (v == 1) { ventiloAuto = false; setFans(0,   0  ); }
        else if (v == 2) { ventiloAuto = false; setFans(220, 220); }
    }

    // Électrovanne (0=auto, 1=OFF forcé, 2=ON forcé)
    database.get(fbClient, DATABASE_URL "/controls/vanne", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if      (v == 0) { vanneAuto = true; }
        else if (v == 1) { vanneAuto = false; digitalWrite(PIN_VANNE, LOW); }
        else if (v == 2) { vanneAuto = false; digitalWrite(PIN_VANNE, HIGH); }
    }

    // Lumière nocturne (0=auto, 1=ON forcé, 2=OFF forcé)
    database.get(fbClient, DATABASE_URL "/controls/lumiere", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if      (v == 1) setNightLight(true);
        else if (v == 2) setNightLight(false);
        // 0 = contrôle par l'horaire automatique
    }

    // Servo (angle 0–180 depuis le site web)
    database.get(fbClient, DATABASE_URL "/controls/servo", res);
    if (res.available()) {
        int angle = res.to<RealtimeDatabaseResult>().to<int>();
        setServo(angle);
    }
}

// ════════════════════════════════════════════════
//  LCD
// ════════════════════════════════════════════════
void updateLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Ph:"); lcd.print(currentPhase);
    lcd.print(" T:"); lcd.print(lastT, 1);
    lcd.setCursor(0, 1);
    lcd.print("H:"); lcd.print((int)lastH);
    lcd.print("% ");
    if      (alarmTemp) lcd.print("T!");
    else if (alarmCO2)  lcd.print("C!");
    else                lcd.print("OK");
}

// ════════════════════════════════════════════════
//  SERIAL MONITOR
// ════════════════════════════════════════════════
void printStatus() {
    Serial.println("\n+----------------------------------+");
    Serial.printf( "| Phase:%d  T:%.1fC  H:%.0f%%\n", currentPhase, lastT, lastH);
    Serial.printf( "| CO2(ADC):%d  Seuil:%d\n", co2Raw, MQ7_THRESHOLD);
    Serial.printf( "| VentiloA:%d/255  VentiloB:%d/255\n", fanSpeedA, fanSpeedB);
    Serial.printf( "| Vanne:%s  Servo:%d\n",
                   digitalRead(PIN_VANNE) ? "ON" : "OFF", servoAngle);
    Serial.printf( "| Lumiere:%s  Firebase:%s\n",
                   nightLightOn ? "ON" : "OFF", fbConnected ? "OK" : "OFF");
    Serial.printf( "| AlrmTemp:%s  AlrmCO2:%s\n",
                   alarmTemp ? "OUI!" : "non", alarmCO2 ? "OUI!" : "non");
    Serial.printf( "| targetT:%.1f  targetH:%.1f\n",
                   PHASES[currentPhase].targetT, PHASES[currentPhase].targetH);
    Serial.printf( "| WiFi IP:%s\n", WiFi.localIP().toString().c_str());
    Serial.println("+----------------------------------+");
}

// ════════════════════════════════════════════════
//  CONNEXION WIFI — Sans bloquer le WDT
// ════════════════════════════════════════════════
void connectWiFi() {
    Serial.printf("\n[WiFi] Connexion au hotspot \"%s\"...\n", WIFI_SSID);
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi connexion");
    lcd.setCursor(0,1); lcd.print(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Attente max 20 secondes, watchdog nourri à chaque itération
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        esp_task_wdt_reset();   // ← nourrir le chien de garde
        Serial.print(".");
        tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        fbConnected = true;
        Serial.println("\n[WiFi] Connecte !");
        Serial.println("[WiFi] IP : " + WiFi.localIP().toString());
        lcd.clear();
        lcd.setCursor(0,0); lcd.print("WiFi OK!");
        lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString());

        // NTP — synchronisation heure Madagascar
        configTime(GMT_OFFSET_S, DAYLIGHT_S, NTP_SERVER);
        Serial.println("[NTP] Sync heure Madagascar UTC+3...");

    } else {
        fbConnected = false;
        Serial.println("\n[WiFi] ECHEC connexion !");
        Serial.println("[WiFi] Verifiez : SSID, mot de passe, hotspot allume");
        lcd.clear();
        lcd.setCursor(0,0); lcd.print("WiFi ECHEC!");
        lcd.setCursor(0,1); lcd.print("Mode local");
    }
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n+================================+");
    Serial.println("|  MycoControl Groupe 2 FINAL   |");
    Serial.println("|  ESP32-S3                     |");
    Serial.println("+================================+\n");

    // ── GPIO Sorties ──────────────────────────────────────────
    pinMode(PIN_BUZZER,   OUTPUT); digitalWrite(PIN_BUZZER,   LOW);
    pinMode(PIN_LUMIERED, OUTPUT); digitalWrite(PIN_LUMIERED, LOW);
    pinMode(PIN_VANNE,    OUTPUT); digitalWrite(PIN_VANNE,    LOW);
    pinMode(PIN_IN1,      OUTPUT); digitalWrite(PIN_IN1,      LOW);
    pinMode(PIN_IN2,      OUTPUT); digitalWrite(PIN_IN2,      LOW);
    pinMode(PIN_IN3,      OUTPUT); digitalWrite(PIN_IN3,      LOW);
    pinMode(PIN_IN4,      OUTPUT); digitalWrite(PIN_IN4,      LOW);

    // ── L298N PWM — API core 3.x : ledcAttach(PIN, freq, bits) ──
    // ⚠️ Plus de canal numérique, on passe directement le PIN
    ledcAttach(PIN_ENA, 1000, 8);
    ledcAttach(PIN_ENB, 1000, 8);
    ledcWrite(PIN_ENA, 0);
    ledcWrite(PIN_ENB, 0);
    Serial.println("[INIT] L298N PWM OK  ENA=GPIO25  ENB=GPIO14");

    // ── Servomoteur ───────────────────────────────────────────
    myServo.attach(PIN_SERVO);
    myServo.write(0);
    Serial.println("[INIT] Servo OK  GPIO33");

    // ── MQ7 ADC ───────────────────────────────────────────────
    // GPIO 36 = entrée ADC uniquement sur ESP32-S3, pas de conflit
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    Serial.printf("[INIT] MQ7 OK  GPIO%d  Seuil=%d\n", PIN_MQ7, MQ7_THRESHOLD);

    // ── DHT22 ─────────────────────────────────────────────────
    dht.begin();
    Serial.println("[INIT] DHT22 OK  GPIO4");

    // ── LCD ───────────────────────────────────────────────────
    lcd.begin();
    lcd.backlight();
    lcd.setCursor(0,0); lcd.print("MycoControl v2");
    lcd.setCursor(0,1); lcd.print("Demarrage...");
    delay(800);

    // ── Bip démarrage (non bloquant) ──────────────────────────
    // 1 bip court manuel ici car buzzerStart() nécessite loop()
    digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW);
    Serial.println("[INIT] Buzzer OK  GPIO15");

    // ── Connexion WiFi hotspot ─────────────────────────────────
    connectWiFi();

    // ── Firebase (NoAuth) ─────────────────────────────────────
    ssl_client.setInsecure();
    initializeApp(fbClient, fbApp, getAuth(no_auth));
    fbApp.getApp<RealtimeDatabase>(database);
    database.url(DATABASE_URL);
    Serial.println("[INIT] Firebase OK (NoAuth)");

    // ── Première lecture DHT22 ────────────────────────────────
    delay(2000);   // DHT22 a besoin de 2s après démarrage
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { lastT = t; lastH = h; }

    lcd.clear();
    lcd.setCursor(0,0); lcd.print(fbConnected ? "Firebase OK" : "Mode local");
    lcd.setCursor(0,1);
    if (!isnan(t)) { lcd.print("T:"); lcd.print(t,1); lcd.print("C"); }
    else            { lcd.print("DHT: attente..."); }

    Serial.println("\n[INIT] Systeme pret !\n");
    Serial.printf("[INFO] SSID: %s\n", WIFI_SSID);
    Serial.printf("[INFO] Hotspot: Activez-le sur votre telephone\n\n");
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── OBLIGATOIRE — FirebaseClient traite ses réponses ────────
    fbApp.loop();
    database.loop();

    // ── Buzzer non bloquant ────────────────────────────────────
    buzzerTick();

    // ── 1. Capteurs toutes les 2s ──────────────────────────────
    if (now - tmrSensor >= INTERVAL_SENSOR) {
        tmrSensor = now;

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            lastT = t;
            lastH = h;
        } else {
            Serial.println("[DHT22] Erreur lecture !");
        }

        co2Raw = readCO2();
        controlAutomation(lastT, lastH);
    }

    // ── 2. Écriture Firebase toutes les 3s ────────────────────
    if (now - tmrFbWrite >= INTERVAL_FB_WRITE) {
        tmrFbWrite = now;
        writeToFirebase();
    }

    // ── 3. Lecture commandes web toutes les 4s ─────────────────
    if (now - tmrFbRead >= INTERVAL_FB_READ) {
        tmrFbRead = now;
        readFromFirebase();
    }

    // ── 4. Horaire lumière nocturne toutes les 5s ──────────────
    if (now - tmrSchedule >= INTERVAL_SCHEDULE) {
        tmrSchedule = now;
        controlNightSchedule();
    }

    // ── 5. Affichage Serial + LCD toutes les 4s ────────────────
    if (now - tmrPrint >= INTERVAL_PRINT) {
        tmrPrint = now;
        printStatus();
        updateLCD();
    }

    // ── 6. Vérification WiFi toutes les 15s ───────────────────
    // Si le hotspot était éteint au démarrage, se reconnecte automatiquement
    if (now - tmrWifi >= INTERVAL_WIFI_CHK) {
        tmrWifi = now;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Reconnexion...");
            fbConnected = false;
            WiFi.disconnect();
            delay(500);
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            // Vérification au prochain cycle
        } else if (!fbConnected) {
            fbConnected = true;
            Serial.println("[WiFi] Reconnecte !");
        }
    }
}
