#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include "DHT.h"
#include <LiquidCrystal_I2C.h>
#include <time.h>

// ════════════════════════════════════════════════
//  CONFIGURATION RÉSEAU & FIREBASE
// ════════════════════════════════════════════════
const char* ssid     = "webchampignon";
const char* password = "12345678";

#define DATABASE_URL "https://webchampignon-default-rtdb.firebaseio.com"

// NTP — Madagascar UTC+3
#define NTP_SERVER   "pool.ntp.org"
#define GMT_OFFSET_S 10800   // UTC+3
#define DAYLIGHT_S   0

// ════════════════════════════════════════════════
//  BROCHAGE GPIO
// ════════════════════════════════════════════════
#define DHTPIN        4    // DHT22 DATA
#define DHTTYPE       DHT22

#define PIN_VENTILO   5    // Relais Ventilateur
#define PIN_VANNE     48   // Relais Électrovanne
#define PIN_BUZZER    15   // Buzzer actif (+)
#define PIN_LUMIERED  2    // Lumière nocturne LED rouge

// ════════════════════════════════════════════════
//  HORAIRE LUMIÈRE NOCTURNE
// ════════════════════════════════════════════════
#define NIGHT_ON_HOUR   17
#define NIGHT_ON_MIN    30
#define NIGHT_OFF_HOUR   6
#define NIGHT_OFF_MIN    0

// ════════════════════════════════════════════════
//  OBJETS
// ════════════════════════════════════════════════
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

WiFiClientSecure ssl_client;
AsyncClientClass client(ssl_client);
FirebaseApp app;
RealtimeDatabase database;
AsyncResult result;
NoAuth no_auth;

// ════════════════════════════════════════════════
//  ÉTAT GLOBAL
// ════════════════════════════════════════════════
int   currentPhase   = 1;
bool  alarmActive    = false;
bool  nightLightOn   = false;
bool  ventiloOn      = false;
bool  vanneOn        = false;

// Timers logiciels
unsigned long lastSensorMs  = 0;
unsigned long lastFbReadMs  = 0;
unsigned long lastScheduleMs= 0;
#define INTERVAL_SENSOR   2000
#define INTERVAL_FB_READ  3000
#define INTERVAL_SCHEDULE 5000

// ════════════════════════════════════════════════
//  BUZZER : N bips
// ════════════════════════════════════════════════
void buzzerBip(int n, int dureeMs = 200, int pauseMs = 100) {
    for (int i = 0; i < n; i++) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(dureeMs);
        digitalWrite(PIN_BUZZER, LOW);
        if (i < n - 1) delay(pauseMs);
    }
}

// ════════════════════════════════════════════════
//  HEURE LOCALE (NTP)
// ════════════════════════════════════════════════
struct tm getTime() {
    struct tm ti = {};
    time_t now = time(nullptr);
    localtime_r(&now, &ti);
    return ti;
}

// Retourne vrai si l'heure est dans la plage [onH:onM, offH:offM[
// Gère le cas qui enjambe minuit (17h30 → 06h00)
bool isInRange(int onH, int onM, int offH, int offM) {
    struct tm ti = getTime();
    int now_min  = ti.tm_hour * 60 + ti.tm_min;
    int on_min   = onH  * 60 + onM;
    int off_min  = offH * 60 + offM;
    if (on_min < off_min) {
        return (now_min >= on_min && now_min < off_min);
    } else {
        // Enjambe minuit (ex: 17:30 → 06:00)
        return (now_min >= on_min || now_min < off_min);
    }
}

// ════════════════════════════════════════════════
//  AUTOMATION — Ventilateur & Électrovanne
// ════════════════════════════════════════════════
void controlAutomation(int phase, float t, float h) {
    float targetT, targetH;
    switch (phase) {
        case 1: targetT = 26.5; targetH = 65.0; break; // Préparation
        case 2: targetT = 25.0; targetH = 87.5; break; // Incubation
        case 3: targetT = 22.0; targetH = 92.0; break; // Fructification
        case 4: targetT =  6.0; targetH = 80.0; break; // Récolte
        default: targetT = 25.0; targetH = 80.0; break;
    }

    // ── Ventilateur : ON si température dépasse la cible ──────
    ventiloOn = (t > targetT);
    digitalWrite(PIN_VENTILO, ventiloOn ? HIGH : LOW);

    // ── Électrovanne : ON si humidité trop basse ──────────────
    vanneOn = (h < targetH);
    digitalWrite(PIN_VANNE, vanneOn ? HIGH : LOW);

    // ── ALARME : température dépasse la cible + 2°C ───────────
    bool tempAlarm = (t > targetT + 2.0);
    if (tempAlarm && !alarmActive) {
        alarmActive = true;
        Serial.printf("[ALARME] Temp critique : %.1f°C (cible : %.1f°C)\n", t, targetT);
        buzzerBip(3, 300, 150); // 3 bips longs
        // Écrire sur Firebase pour alerter le dashboard web
        database.set(client, DATABASE_URL "/alarms/temp", String("1"));
        // On crée d'abord la phrase dans une variable propre
        String messageAlarme = String("ALARME Temp: ") + String(t, 1) + "C > " + String(targetT + 2.0, 1) + "C";
        database.set(client, DATABASE_URL "/alarms/message", messageAlarme);
    } else if (!tempAlarm && alarmActive) {
        alarmActive = false;
        database.set(client, DATABASE_URL "/alarms/temp", String("0"));
        database.set(client, DATABASE_URL "/alarms/message", String("OK"));
        buzzerBip(1, 80); // bip court fin d'alarme
    }

    // ── Envoyer état actionneurs vers Firebase ─────────────────
    database.set(client, DATABASE_URL "/status/ventilo", String(ventiloOn ? 1 : 0));
    database.set(client, DATABASE_URL "/status/vanne",   String(vanneOn   ? 1 : 0));
    database.set(client, DATABASE_URL "/status/alarm",   String(alarmActive ? 1 : 0));
    database.set(client, DATABASE_URL "/status/targetT", String(targetT, 1));
    database.set(client, DATABASE_URL "/status/targetH", String(targetH, 1));
}

// ════════════════════════════════════════════════
//  LUMIÈRE NOCTURNE — Horaire 17h30 → 06h00
// ════════════════════════════════════════════════
void controlNightLight() {
    bool shouldBeOn = isInRange(NIGHT_ON_HOUR, NIGHT_ON_MIN,
                                NIGHT_OFF_HOUR, NIGHT_OFF_MIN);

    if (shouldBeOn != nightLightOn) {
        nightLightOn = shouldBeOn;
        digitalWrite(PIN_LUMIERED, nightLightOn ? HIGH : LOW);

        struct tm ti = getTime();
        Serial.printf("[NUIT] Lumière nocturne → %s (%02d:%02d)\n",
                      nightLightOn ? "ON" : "OFF", ti.tm_hour, ti.tm_min);

        database.set(client, DATABASE_URL "/status/nightlight",
                     String(nightLightOn ? 1 : 0));
    }
}

// ════════════════════════════════════════════════
//  LCD
// ════════════════════════════════════════════════
void updateLCD(float t, float h) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Ph:"); lcd.print(currentPhase);
    lcd.print(" T:"); lcd.print(t, 1); lcd.print("C");
    lcd.setCursor(0, 1);
    lcd.print("H:"); lcd.print(h, 0); lcd.print("%");
    lcd.print(alarmActive ? " ALARM" : "  Auto");
}

// ════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== MycoControl Groupe 2 ===");

    // Sorties
    pinMode(PIN_VENTILO,  OUTPUT); digitalWrite(PIN_VENTILO,  LOW);
    pinMode(PIN_VANNE,    OUTPUT); digitalWrite(PIN_VANNE,    LOW);
    pinMode(PIN_BUZZER,   OUTPUT); digitalWrite(PIN_BUZZER,   LOW);
    pinMode(PIN_LUMIERED, OUTPUT); digitalWrite(PIN_LUMIERED, LOW);

    // LCD
    lcd.begin();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("MycoControl v2");
    lcd.setCursor(0, 1); lcd.print("Demarrage...");

    // DHT22
    dht.begin();

    // WiFi en mode Access Point
    WiFi.softAP(ssid, password);
    Serial.println("[WiFi] AP démarré → " + WiFi.softAPIP().toString());

    // Connexion additionnelle en STA pour NTP (optionnel)
    // Si vous avez une connexion internet :
    // WiFi.begin("VotreSSID", "VotreMotDePasse");
    // while (WiFi.status() != WL_CONNECTED) delay(500);
    // configTime(GMT_OFFSET_S, DAYLIGHT_S, NTP_SERVER);

    // Firebase (NoAuth)
    ssl_client.setInsecure();
    initializeApp(client, app, getAuth(no_auth));

    delay(1000);
    buzzerBip(2, 100, 80); // 2 bips démarrage

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi AP pret");
    lcd.setCursor(0, 1); lcd.print("Firebase OK");

    Serial.println("[INIT] Système prêt");
}

// ════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── Lecture Firebase : phase demandée par le web ───────────
    if (now - lastFbReadMs >= INTERVAL_FB_READ) {
        lastFbReadMs = now;
        database.get(client, DATABASE_URL "/config/currentPhase", result);
        if (result.available()) {
            int ph = result.to<RealtimeDatabaseResult>().to<int>();
            if (ph >= 1 && ph <= 4 && ph != currentPhase) {
                currentPhase = ph;
                Serial.printf("[FB] Phase → %d\n", currentPhase);
            }
        }

        // Lecture commande manuelle ventilateur depuis web
        AsyncResult res2;
        database.get(client, DATABASE_URL "/controls/ventilo", res2);
        if (res2.available()) {
            int v = res2.to<RealtimeDatabaseResult>().to<int>();
            // Si le web force ON (v==2), ignorer l'automatique
            if (v == 2) { digitalWrite(PIN_VENTILO, HIGH); }
            else if (v == 0) { digitalWrite(PIN_VENTILO, LOW); }
            // v == 1 = auto (controlAutomation gère)
        }
    }

    // ── Lecture capteurs + automation ─────────────────────────
    if (now - lastSensorMs >= INTERVAL_SENSOR) {
        lastSensorMs = now;
        float h = dht.readHumidity();
        float t = dht.readTemperature();

        if (!isnan(h) && !isnan(t)) {
            // Envoi Firebase
            database.set(client, DATABASE_URL "/status/temp", String(t, 1));
            database.set(client, DATABASE_URL "/status/hum",  String(h, 1));

            // Automation ventilateur + électrovanne + alarme
            controlAutomation(currentPhase, t, h);

            // Mise à jour LCD
            updateLCD(t, h);

            Serial.printf("[SENSOR] T:%.1f H:%.1f Ph:%d Fan:%s Vanne:%s Alarm:%s\n",
                t, h, currentPhase,
                ventiloOn   ? "ON" : "OFF",
                vanneOn     ? "ON" : "OFF",
                alarmActive ? "OUI" : "NON");
        } else {
            Serial.println("[DHT22] Erreur lecture !");
        }
    }

    // ── Horaire lumière nocturne (vérif toutes les 5 sec) ─────
    if (now - lastScheduleMs >= INTERVAL_SCHEDULE) {
        lastScheduleMs = now;
        controlNightLight();
    }


// --- LECTURE DES COMMANDES MANUELLES ---

// On vérifie l'état de la Vanne
if (Firebase.RTDB.getInt(&fbdo, "/controls/vanne")) {
    int etatVanne = fbdo.intData();
    digitalWrite(12, etatVanne); // Supposons que la vanne est sur le PIN 12
}

// On vérifie l'état de la Lumière
if (Firebase.RTDB.getInt(&fbdo, "/controls/lumiere")) {
    int etatLumiere = fbdo.intData();
    digitalWrite(13, etatLumiere); // Supposons la lumière sur le PIN 13
}

// On vérifie l'état du Ventilateur
if (Firebase.RTDB.getInt(&fbdo, "/controls/ventilo")) {
    int etatVentilo = fbdo.intData();
    digitalWrite(14, etatVentilo); // Supposons le ventilo sur le PIN 14
}
}

/*
 ════════════════════════════════════════════════════
  FIREBASE RTDB — Chemins utilisés
 ════════════════════════════════════════════════════
  /config/currentPhase    int     Phase active (1–4) — Web → ESP32
  /controls/ventilo       int     0=auto 1=auto 2=forcé ON — Web → ESP32
  /status/temp            String  Température DHT22 (°C)
  /status/hum             String  Humidité DHT22 (%)
  /status/ventilo         String  0 ou 1
  /status/vanne           String  0 ou 1
  /status/nightlight      String  0 ou 1
  /status/alarm           String  0 ou 1
  /status/targetT         String  Cible température phase active
  /status/targetH         String  Cible humidité phase active
  /alarms/temp            String  0 ou 1
  /alarms/message         String  Message d'alarme

 BROCHAGE GPIO
 ─────────────────────────────────────────────────
  GPIO  4  → DHT22 DATA
  GPIO  5  → Relais Ventilateur
  GPIO 48  → Relais Électrovanne
  GPIO 15  → Buzzer actif (+)
  GPIO  2  → Lumière nocturne LED rouge
  I²C      → LCD 16×2 (adresse 0x27)
 ════════════════════════════════════════════════════
*/
