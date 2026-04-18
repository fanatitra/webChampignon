/*************************************************************
 *  MycoControl Groupe 2 — ESP32 FINAL CORRIGÉ
 *  ─────────────────────────────────────────────────────────
 *  Capteurs  : DHT22 (temp/hum)  GPIO 4
 *              MQ7    (CO2 gaz)  GPIO 34 (ADC)
 *  Actionneurs :
 *    L298N  ENA (Ventilo A PWM)  GPIO 25
 *    L298N  IN1                  GPIO 26
 *    L298N  IN2                  GPIO 27
 *    L298N  ENB (Ventilo B PWM)  GPIO 14
 *    L298N  IN3                  GPIO 12
 *    L298N  IN4                  GPIO 13
 *    Servomoteur                 GPIO 33
 *    Électrovanne (relais)       GPIO 48
 *    Lumière nocturne LED rouge  GPIO 2
 *    Buzzer actif (+)            GPIO 15
 *    LCD 16×2 I²C                0x27
 *
 *  Firebase RTDB — webchampignon
 *  /config/currentPhase    (int)    Web → ESP32
 *  /controls/ventilo       (int 0/1/2) Web → ESP32
 *  /controls/vanne         (int 0/1)   Web → ESP32
 *  /controls/lumiere       (int 0/1)   Web → ESP32
 *  /controls/servo         (int 0–180) Web → ESP32
 *  /sensor/temperature     (float)   ESP32 → Web (lecture)
 *  /sensor/humidity        (float)   ESP32 → Web
 *  /status/temp            (String)  ESP32 → Web
 *  /status/hum             (String)  ESP32 → Web
 *  /status/co2             (String)  ESP32 → Web
 *  /status/ventilo         (String 0/1) ESP32 → Web
 *  /status/vanne           (String 0/1) ESP32 → Web
 *  /status/nightlight      (String 0/1) ESP32 → Web
 *  /status/servo           (String)  ESP32 → Web
 *  /status/targetT         (String)  ESP32 → Web
 *  /status/targetH         (String)  ESP32 → Web
 *  /alarms/temp            (String 0/1) ESP32 → Web
 *  /alarms/co2             (String 0/1) ESP32 → Web
 *  /alarms/message         (String)  ESP32 → Web
 *
 *  WiFi : Access Point  SSID=webchampignon  PASS=12345678
 *  Connectez l'ESP32 ET le PC/téléphone sur ce réseau
 *  puis accédez à Firebase via Internet
 *
 *  Bibliothèques nécessaires :
 *    FirebaseClient  (Mobizt)
 *    DHT sensor library (Adafruit)
 *    LiquidCrystal I2C
 *    ESP32Servo
 *************************************************************/

#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include "DHT.h"
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// ════════════════════════════════════════════════
//  RÉSEAU & FIREBASE
// ════════════════════════════════════════════════
const char* AP_SSID = "webchampignon";
const char* AP_PASS = "12345678";

// Si vous avez du WiFi Internet (pour Firebase) :
const char* STA_SSID = "webchampignon";   // ← même réseau ou routeur
const char* STA_PASS = "12345678";

#define DATABASE_URL "https://webchampignon-default-rtdb.firebaseio.com"

// ════════════════════════════════════════════════
//  GPIO
// ════════════════════════════════════════════════
// Capteurs
#define PIN_DHT      4
#define DHTTYPE      DHT22
#define PIN_MQ7      34    // ADC — sortie analogique MQ7

// L298N — Ventilateur A
#define PIN_ENA      25    // PWM vitesse ventilo A
#define PIN_IN1      26    // Direction ventilo A
#define PIN_IN2      27

// L298N — Ventilateur B
#define PIN_ENB      14    // PWM vitesse ventilo B
#define PIN_IN3      12    // Direction ventilo B
#define PIN_IN4      13

// Servomoteur
#define PIN_SERVO    33

// Électrovanne (relais actif HIGH)
#define PIN_VANNE    48

// Lumière nocturne
#define PIN_LUMIERED  2

// Buzzer
#define PIN_BUZZER   15

// ════════════════════════════════════════════════
//  SEUILS CO2 MQ7
// ════════════════════════════════════════════════
#define CO2_ALARM_PPM  400   // Seuil alarme CO2 (valeur ADC brute ~2000 ≈ danger)
// Le MQ7 donne une tension analogique : plus c'est élevé = plus de CO
// Calibrez selon votre capteur. Valeur 0–4095 (ADC 12 bits ESP32)
#define MQ7_THRESHOLD  2000  // Valeur ADC au-delà de laquelle alarme CO

// ════════════════════════════════════════════════
//  PARAMÈTRES PAR PHASE
// ════════════════════════════════════════════════
struct PhaseConfig {
    float targetT;
    float targetH;
    int   servoAngle;    // angle servo automatique pour cette phase
    int   fanSpeed;      // vitesse L298N 0–255
};

const PhaseConfig PHASES[5] = {
    {},                                     // index 0 non utilisé
    {26.5f, 65.0f,  0,  150},             // Phase 1 — Préparation
    {25.0f, 87.5f,  45, 180},             // Phase 2 — Incubation
    {22.0f, 92.0f,  90, 220},             // Phase 3 — Fructification
    { 6.0f, 80.0f, 135, 100},             // Phase 4 — Récolte
};

// ════════════════════════════════════════════════
//  OBJETS
// ════════════════════════════════════════════════
DHT              dht(PIN_DHT, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo            myServo;

WiFiClientSecure ssl_client;
AsyncClientClass client(ssl_client);
FirebaseApp      app;
RealtimeDatabase database;
AsyncResult      result;
NoAuth           no_auth;

// ════════════════════════════════════════════════
//  ÉTAT GLOBAL
// ════════════════════════════════════════════════
int   currentPhase  = 1;
bool  alarmTemp     = false;
bool  alarmCO2      = false;
bool  nightLightOn  = false;
bool  ventiloAuto   = true;   // true = auto, false = manuel web
bool  vanneAuto     = true;

int   fanSpeedA     = 0;      // vitesse PWM ventilo A (0–255)
int   fanSpeedB     = 0;      // vitesse PWM ventilo B
int   servoAngle    = 0;      // angle actuel servo
int   co2Raw        = 0;      // valeur ADC MQ7

float lastT = 0, lastH = 0;

// Timers
unsigned long tmrSensor   = 0;
unsigned long tmrFbWrite  = 0;
unsigned long tmrFbRead   = 0;
unsigned long tmrSchedule = 0;
unsigned long tmrPrint    = 0;

#define INTERVAL_SENSOR    2000
#define INTERVAL_FB_WRITE  2500
#define INTERVAL_FB_READ   3000
#define INTERVAL_SCHEDULE  5000
#define INTERVAL_PRINT     3000

// ════════════════════════════════════════════════
//  FONCTIONS UTILITAIRES
// ════════════════════════════════════════════════

void buzzerBip(int n, int ms = 200, int pause = 100) {
    for (int i = 0; i < n; i++) {
        digitalWrite(PIN_BUZZER, HIGH); delay(ms);
        digitalWrite(PIN_BUZZER, LOW);
        if (i < n-1) delay(pause);
    }
}

// L298N — régler vitesse ventilateurs A et B
void setFans(int speedA, int speedB) {
    fanSpeedA = constrain(speedA, 0, 255);
    fanSpeedB = constrain(speedB, 0, 255);

    if (fanSpeedA > 0) {
        digitalWrite(PIN_IN1, HIGH);
        digitalWrite(PIN_IN2, LOW);
        ledcWrite(0, fanSpeedA);   // canal 0 = ENA
    } else {
        digitalWrite(PIN_IN1, LOW);
        digitalWrite(PIN_IN2, LOW);
        ledcWrite(0, 0);
    }

    if (fanSpeedB > 0) {
        digitalWrite(PIN_IN3, HIGH);
        digitalWrite(PIN_IN4, LOW);
        ledcWrite(1, fanSpeedB);   // canal 1 = ENB
    } else {
        digitalWrite(PIN_IN3, LOW);
        digitalWrite(PIN_IN4, LOW);
        ledcWrite(1, 0);
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
//  LECTURE MQ7 CO2
// ════════════════════════════════════════════════
int readCO2() {
    // Moyenne 5 lectures pour stabilité
    long sum = 0;
    for (int i = 0; i < 5; i++) { sum += analogRead(PIN_MQ7); delay(10); }
    return (int)(sum / 5);
}

// ════════════════════════════════════════════════
//  AUTOMATION — selon phase
// ════════════════════════════════════════════════
void controlAutomation(float t, float h) {
    if (currentPhase < 1 || currentPhase > 4) return;
    const PhaseConfig& cfg = PHASES[currentPhase];

    // ── Ventilateurs (si mode auto) ────────────────────────────
    if (ventiloAuto) {
        if (t > cfg.targetT) {
            setFans(cfg.fanSpeed, cfg.fanSpeed);
        } else {
            setFans(0, 0);
        }
    }

    // ── Électrovanne (si mode auto) ────────────────────────────
    if (vanneAuto) {
        bool shouldOpen = (h < cfg.targetH);
        digitalWrite(PIN_VANNE, shouldOpen ? HIGH : LOW);
    }

    // ── Servo automatique ──────────────────────────────────────
    // Ajuste l'angle selon si ventilo actif ou non
    if (fanSpeedA > 0) {
        setServo(cfg.servoAngle);       // ouverture pour ventilation
    } else {
        setServo(0);                    // fermé au repos
    }

    // ── Alarme température ──────────────────────────────────────
    bool newTempAlarm = (t > cfg.targetT + 2.0f);
    if (newTempAlarm && !alarmTemp) {
        alarmTemp = true;
        Serial.printf("🚨 [ALARME TEMP] %.1f°C > %.1f°C\n", t, cfg.targetT + 2.0f);
        buzzerBip(3, 400, 150);
    } else if (!newTempAlarm && alarmTemp) {
        alarmTemp = false;
        buzzerBip(1, 80);
        Serial.println("✅ [ALARME TEMP] Résolue");
    }

    // ── Alarme CO2 (MQ7) ───────────────────────────────────────
    bool newCO2Alarm = (co2Raw > MQ7_THRESHOLD);
    if (newCO2Alarm && !alarmCO2) {
        alarmCO2 = true;
        Serial.printf("🚨 [ALARME CO2] ADC=%d > seuil %d\n", co2Raw, MQ7_THRESHOLD);
        buzzerBip(5, 200, 80);
        // Si CO2 élevé → ventiler à fond
        setFans(255, 255);
    } else if (!newCO2Alarm && alarmCO2) {
        alarmCO2 = false;
        Serial.println("✅ [ALARME CO2] Résolue");
    }
}

// ════════════════════════════════════════════════
//  LUMIÈRE NOCTURNE — 17h30 → 06h00
// ════════════════════════════════════════════════
void controlNightSchedule() {
    // Horloge locale (millis fallback si pas de NTP)
    // Pour NTP : connecter en mode STA à un routeur internet
    // Ici on utilise une horloge interne basée sur millis
    // Pour activer NTP, décommentez dans setup()
    
    // Heure locale depuis NTP (si dispo)
    struct tm ti = {};
    time_t now   = time(nullptr);
    if (now < 1000000) return;   // NTP pas encore synchronisé
    localtime_r(&now, &ti);

    int nowMin = ti.tm_hour * 60 + ti.tm_min;
    int onMin  = 17 * 60 + 30;   // 17:30
    int offMin =  6 * 60 +  0;   // 06:00

    // Plage qui enjambe minuit
    bool shouldBeOn = (nowMin >= onMin || nowMin < offMin);

    if (shouldBeOn != nightLightOn) {
        setNightLight(shouldBeOn);
        Serial.printf("[NUIT] Lumière → %s (%02d:%02d)\n",
            shouldBeOn ? "ON" : "OFF", ti.tm_hour, ti.tm_min);
    }
}

// ════════════════════════════════════════════════
//  ÉCRITURE FIREBASE — Données capteurs & états
// ════════════════════════════════════════════════
void writeToFirebase() {
    // /sensor/ (format que le web lit en premier d'après votre FB)
    database.set(client, DATABASE_URL "/sensor/temperature", String(lastT, 1));
    database.set(client, DATABASE_URL "/sensor/humidity",    String(lastH, 1));

    // /status/ (tous les états)
    database.set(client, DATABASE_URL "/status/temp",       String(lastT, 1));
    database.set(client, DATABASE_URL "/status/hum",        String(lastH, 1));
    database.set(client, DATABASE_URL "/status/co2",        String(co2Raw));
    database.set(client, DATABASE_URL "/status/ventilo",    String(fanSpeedA > 0 ? 1 : 0));
    database.set(client, DATABASE_URL "/status/vanne",      String(digitalRead(PIN_VANNE)));
    database.set(client, DATABASE_URL "/status/nightlight", String(nightLightOn ? 1 : 0));
    database.set(client, DATABASE_URL "/status/servo",      String(servoAngle));
    database.set(client, DATABASE_URL "/status/fanSpeedA",  String(fanSpeedA));
    database.set(client, DATABASE_URL "/status/fanSpeedB",  String(fanSpeedB));

    if (currentPhase >= 1 && currentPhase <= 4) {
        database.set(client, DATABASE_URL "/status/targetT", String(PHASES[currentPhase].targetT, 1));
        database.set(client, DATABASE_URL "/status/targetH", String(PHASES[currentPhase].targetH, 1));
    }

    // /alarms/
    database.set(client, DATABASE_URL "/alarms/temp",    String(alarmTemp  ? 1 : 0));
    database.set(client, DATABASE_URL "/alarms/co2",     String(alarmCO2   ? 1 : 0));

    String msg = "Système OK";
    if (alarmTemp && alarmCO2) msg = "ALARME TEMP + CO2 !";
    else if (alarmTemp)        msg = "ALARME Temp: " + String(lastT,1) + "C";
    else if (alarmCO2)         msg = "ALARME CO2: " + String(co2Raw);
    database.set(client, DATABASE_URL "/alarms/message", msg);
}

// ════════════════════════════════════════════════
//  LECTURE FIREBASE — Commandes Web → ESP32
// ════════════════════════════════════════════════
void readFromFirebase() {
    AsyncResult res;

    // ── Phase ──────────────────────────────────────────────────
    database.get(client, DATABASE_URL "/config/currentPhase", res);
    if (res.available()) {
        int ph = res.to<RealtimeDatabaseResult>().to<int>();
        if (ph >= 1 && ph <= 4 && ph != currentPhase) {
            currentPhase = ph;
            Serial.printf("[FB] Phase changée → %d\n", currentPhase);
            // Appliquer immédiatement les paramètres
            setServo(PHASES[ph].servoAngle);
        }
    }

    // ── Ventilateur (0=auto, 1=forcé OFF, 2=forcé ON) ─────────
    database.get(client, DATABASE_URL "/controls/ventilo", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if (v == 0) {
            ventiloAuto = true;
            Serial.println("[FB] Ventilo → AUTO");
        } else if (v == 1) {
            ventiloAuto = false;
            setFans(0, 0);
            Serial.println("[FB] Ventilo → FORCÉ OFF");
        } else if (v == 2) {
            ventiloAuto = false;
            setFans(220, 220);
            Serial.println("[FB] Ventilo → FORCÉ ON");
        }
    }

    // ── Électrovanne (0=auto, 1=forcé OFF, 2=forcé ON) ────────
    database.get(client, DATABASE_URL "/controls/vanne", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if (v == 0) {
            vanneAuto = true;
        } else if (v == 1) {
            vanneAuto = false;
            digitalWrite(PIN_VANNE, LOW);
            Serial.println("[FB] Vanne → FORCÉE OFF");
        } else if (v == 2) {
            vanneAuto = false;
            digitalWrite(PIN_VANNE, HIGH);
            Serial.println("[FB] Vanne → FORCÉE ON");
        }
    }

    // ── Lumière nocturne (0=auto, 1=forcé ON, 2=forcé OFF) ────
    database.get(client, DATABASE_URL "/controls/lumiere", res);
    if (res.available()) {
        int v = res.to<RealtimeDatabaseResult>().to<int>();
        if (v == 1) {
            setNightLight(true);
            Serial.println("[FB] Lumière → FORCÉE ON");
        } else if (v == 2) {
            setNightLight(false);
            Serial.println("[FB] Lumière → FORCÉE OFF");
        }
        // v==0 : géré par l'horaire automatique
    }

    // ── Servo (angle 0–180 envoyé par le web) ─────────────────
    database.get(client, DATABASE_URL "/controls/servo", res);
    if (res.available()) {
        int angle = res.to<RealtimeDatabaseResult>().to<int>();
        setServo(angle);
        Serial.printf("[FB] Servo → %d°\n", angle);
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
    lcd.print("H:"); lcd.print(lastH, 0);
    lcd.print("% ");
    if      (alarmTemp) lcd.print("T!");
    else if (alarmCO2)  lcd.print("C!");
    else                lcd.print("OK");
}

// ════════════════════════════════════════════════
//  SERIAL MONITOR — Affichage temps réel
// ════════════════════════════════════════════════
void printStatus() {
    Serial.println(F("\n╔══════════════════════════════════╗"));
    Serial.printf("║  Phase: %d | T: %.1f°C | H: %.0f%%\n", currentPhase, lastT, lastH);
    Serial.printf("║  CO2(ADC): %d | Seuil: %d\n", co2Raw, MQ7_THRESHOLD);
    Serial.printf("║  Ventilo A: %d/255 | B: %d/255\n", fanSpeedA, fanSpeedB);
    Serial.printf("║  Vanne: %s | Servo: %d°\n",
        digitalRead(PIN_VANNE) ? "ON" : "OFF", servoAngle);
    Serial.printf("║  Lumiere nuit: %s\n", nightLightOn ? "ON" : "OFF");
    Serial.printf("║  Alarme Temp: %s | CO2: %s\n",
        alarmTemp ? "OUI 🚨" : "NON", alarmCO2 ? "OUI 🚨" : "NON");
    Serial.printf("║  targetT: %.1f | targetH: %.1f\n",
        PHASES[currentPhase].targetT, PHASES[currentPhase].targetH);
    Serial.println(F("╚══════════════════════════════════╝"));
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println(F("\n╔══════════════════════════════════╗"));
    Serial.println(F("║   MycoControl Groupe 2 — FINAL   ║"));
    Serial.println(F("╚══════════════════════════════════╝\n"));

    // ── Buzzer ─────────────────────────────────────────────────
    pinMode(PIN_BUZZER,   OUTPUT); digitalWrite(PIN_BUZZER,   LOW);

    // ── Lumière nocturne ───────────────────────────────────────
    pinMode(PIN_LUMIERED, OUTPUT); digitalWrite(PIN_LUMIERED, LOW);

    // ── Électrovanne ───────────────────────────────────────────
    pinMode(PIN_VANNE,    OUTPUT); digitalWrite(PIN_VANNE,    LOW);

    // ── L298N — PWM LEDC core 3.x ─────────────────────────────
    pinMode(PIN_IN1, OUTPUT); digitalWrite(PIN_IN1, LOW);
    pinMode(PIN_IN2, OUTPUT); digitalWrite(PIN_IN2, LOW);
    pinMode(PIN_IN3, OUTPUT); digitalWrite(PIN_IN3, LOW);
    pinMode(PIN_IN4, OUTPUT); digitalWrite(PIN_IN4, LOW);

    // Canal 0 = ENA (ventilo A), Canal 1 = ENB (ventilo B)
    ledcAttach(PIN_ENA, 1000, 8);   // GPIO25, 1kHz, 8 bits
    ledcAttach(PIN_ENB, 1000, 8);   // GPIO14
    ledcWrite(PIN_ENA, 0);
    ledcWrite(PIN_ENB, 0);
    Serial.println("[INIT] L298N : OK (ENA=GPIO25, ENB=GPIO14)");

    // ── Servomoteur ────────────────────────────────────────────
    myServo.attach(PIN_SERVO);
    myServo.write(0);
    Serial.println("[INIT] Servo : OK (GPIO33)");

    // ── MQ7 ADC ────────────────────────────────────────────────
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    Serial.printf("[INIT] MQ7 CO2 : GPIO%d (ADC 12 bits)\n", PIN_MQ7);

    // ── DHT22 ──────────────────────────────────────────────────
    dht.begin();
    Serial.println("[INIT] DHT22 : GPIO4");

    // ── LCD ────────────────────────────────────────────────────
    lcd.begin();
    lcd.backlight();
    lcd.setCursor(0,0); lcd.print("MycoControl v2");
    lcd.setCursor(0,1); lcd.print("Demarrage...");

    // ── WiFi STA → Firebase ────────────────────────────────────
    // ESP32 doit être connecté à Internet pour Firebase
    // Si vous utilisez un routeur séparé, mettez ses credentials ici
    Serial.printf("[WiFi] Connexion à \"%s\"...\n", STA_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(STA_SSID, STA_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500); Serial.print("."); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connecté ! IP: " + WiFi.localIP().toString());
        // NTP pour horaire lumière nocturne
        configTime(10800, 0, "pool.ntp.org"); // UTC+3 Madagascar
        Serial.println("[NTP] Sync heure Madagascar UTC+3...");
        delay(2000);
    } else {
        Serial.println("\n[WiFi] ⚠ Pas de connexion Internet — Firebase ne fonctionnera pas");
        Serial.println("[WiFi] Vérifiez SSID/PASS ou utilisez un routeur avec Internet");
        // Démarrer AP en fallback pour que le web fonctionne localement
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial.println("[WiFi] AP démarré → " + WiFi.softAPIP().toString());
    }

    // ── Firebase (NoAuth — correspond à votre config) ──────────
    ssl_client.setInsecure();
    initializeApp(client, app, getAuth(no_auth));
    app.getApp<RealtimeDatabase>(database);
    database.url(DATABASE_URL);
    Serial.println("[Firebase] Init OK (NoAuth)");

    // ── Bip démarrage ──────────────────────────────────────────
    buzzerBip(2, 100, 80);

    lcd.clear();
    lcd.setCursor(0,0); lcd.print("WiFi OK");
    lcd.setCursor(0,1); lcd.print("Firebase pret");

    delay(1000);
    Serial.println("[INIT] ✅ Système prêt\n");
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── 1. Lecture capteurs ────────────────────────────────────
    if (now - tmrSensor >= INTERVAL_SENSOR) {
        tmrSensor = now;

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            lastT = t; lastH = h;
        } else {
            Serial.println("[DHT22] ⚠ Erreur lecture !");
        }

        co2Raw = readCO2();

        // Automation (ventilo + vanne + servo + alarmes)
        controlAutomation(lastT, lastH);
    }

    // ── 2. Écriture Firebase ───────────────────────────────────
    if (now - tmrFbWrite >= INTERVAL_FB_WRITE) {
        tmrFbWrite = now;
        writeToFirebase();
    }

    // ── 3. Lecture commandes Firebase (Web → ESP32) ────────────
    if (now - tmrFbRead >= INTERVAL_FB_READ) {
        tmrFbRead = now;
        readFromFirebase();
    }

    // ── 4. Horaire lumière nocturne ────────────────────────────
    if (now - tmrSchedule >= INTERVAL_SCHEDULE) {
        tmrSchedule = now;
        controlNightSchedule();
    }

    // ── 5. Affichage Serial Monitor ───────────────────────────
    if (now - tmrPrint >= INTERVAL_PRINT) {
        tmrPrint = now;
        printStatus();
        updateLCD();
    }

    // ── 6. Traitement réponses Firebase (obligatoire) ─────────
    // (FirebaseClient traite les callbacks en arrière-plan)
    delay(10);
}
