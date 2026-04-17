#define ENABLE_DATABASE // Indispensable pour RealtimeDatabase.h

#include <Arduino.h>
#include <WiFi.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include "DHT.h"
#include <LiquidCrystal_I2C.h>

// --- CONFIGURATION À REMPLIR ---
//#define WIFI_SSID "webchampignon"
//#define WIFI_PASSWORD "12345678"
#define DATABASE_URL "https://webchampignon-default-rtdb.firebaseio.com" // SANS https://

// --- PINS ---
#define DHTPIN 4
#define DHTTYPE DHT22
#define PIN_VENTILO 5
#define PIN_VANNE 48

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ========================= CONFIGURATION =========================
const char* ssid = "webchampignon";
const char* password = "12345678";


WiFiClientSecure ssl_client;
AsyncClientClass client(ssl_client);
FirebaseApp app;
RealtimeDatabase database;
AsyncResult result;
NoAuth no_auth; 

int currentPhase = 1;

void setup() {
    Serial.begin(115200);
    
    lcd.begin(); 
    lcd.backlight();
    dht.begin();

    pinMode(PIN_VENTILO, OUTPUT);
    pinMode(PIN_VANNE, OUTPUT);

    // --- WiFi AP ---
  WiFi.softAP(ssid, password);
  Serial.println("AP démarré → IP: " + WiFi.softAPIP().toString());

  /*WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connecté");
    */
    ssl_client.setInsecure();
    initializeApp(client, app, getAuth(no_auth)); 
}

void loop() {
    // Lecture de la phase cible
    database.get(client, DATABASE_URL "/config/currentPhase", result);
    
    if (result.available()) {
        currentPhase = result.to<RealtimeDatabaseResult>().to<int>();
    }

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
        controlAutomation(currentPhase, t, h);
        
        // Envoi des données vers Firebase
        database.set(client, DATABASE_URL "/status/temp", String(t, 1));
        database.set(client, DATABASE_URL "/status/hum", String(h, 1));
        
        updateLCD(t, h);
    }
    
    delay(2000);
}

void controlAutomation(int phase, float t, float h) {
    float targetT, targetH;
    switch(phase) {
        case 1: targetT = 26.5; targetH = 65.0; break; 
        case 2: targetT = 25.0; targetH = 87.5; break; 
        case 3: targetT = 22.0; targetH = 92.0; break; 
        case 4: targetT = 6.0;  targetH = 80.0; break; 
        default: targetT = 25.0; targetH = 80.0; break;
    }
    digitalWrite(PIN_VANNE, (h < targetH) ? HIGH : LOW);
    digitalWrite(PIN_VENTILO, (t > targetT) ? HIGH : LOW);
}

void updateLCD(float t, float h) {
    lcd.setCursor(0,0);
    lcd.print("Ph:"); lcd.print(currentPhase);
    lcd.print(" T:"); lcd.print(t, 1);
    lcd.setCursor(0,1);
    lcd.print("H:"); lcd.print(h, 0);
    lcd.print("%  Auto");
}