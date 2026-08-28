/*
  =====================================================================
    Smart Room Monitoring System with ThingSpeak
  TETE/TEEE 3102 -- Assignment 1
        Hardware: Arduino Uno + DHT11/DHT22 + PIR + HC-SR04 Ultrasonic + Buzzer
                  + ESP8266 (ESP-01, AT firmware) as the WiFi module
  =====================================================================

        WHAT THIS SKETCH DOES
        ----------------------
        Every 20 seconds it:
        1. Reads temperature + humidity (DHT), distance (ultrasonic),
             and motion (PIR).
          2. Drives the buzzer locally: ON if motion==1 OR distance<20cm.
    3. Pushes all four readings to a ThingSpeak channel over the
       ESP8266, using plain AT commands (no extra WiFi library needed
       on the Arduino side -- the ESP8266 does the TCP/HTTP work).

        BEFORE YOU FLASH THIS
        ----------------------
        - Install the "DHT sensor library" by Adafruit (Library Manager),
    plus its dependency "Adafruit Unified Sensor".
        - Fill in WIFI_SSID, WIFI_PASS, and TS_API_KEY below (Write API Key
    from your ThingSpeak channel -- see issue #2 on the GitHub Project).
        - Check your ESP8266's AT-firmware baud rate. Factory default is
          often 115200; this sketch assumes it's been set to 9600 with
          AT+UART_DEF=9600,8,1,0,0. If WiFi never connects, that's the
          first thing to check.
        - Wiring below assumes the pin numbers in PIN DEFINITIONS -- adjust
          to match your actual breadboard layout (issue #1).
*/

#include <SoftwareSerial.h>
#include <DHT.h>

// ---------------- PIN DEFINITIONS ----------------
#define DHTPIN      2       // DHT data pin
#define DHTTYPE     DHT11   // change to DHT22 if that's what you have
#define PIR_PIN     3
#define TRIG_PIN    4       // ultrasonic trigger
#define ECHO_PIN    5       // ultrasonic echo
#define BUZZER_PIN  6
#define ESP_RX      7       // Arduino pin that RECEIVES  <- ESP8266 TX
#define ESP_TX      8       // Arduino pin that TRANSMITS -> ESP8266 RX
// NOTE: ESP8266 logic is 3.3V -- use a voltage divider or level
// shifter on this TX->RX line so you don't feed 5V into the module.

DHT dht(DHTPIN, DHTTYPE);
SoftwareSerial esp8266(ESP_RX, ESP_TX);

// ---------------- WIFI / THINGSPEAK ----------------
const char* WIFI_SSID  = "YOUR_WIFI_NAME";
const char* WIFI_PASS  = "YOUR_WIFI_PASSWORD";
const char* TS_API_KEY = "YOUR_THINGSPEAK_WRITE_API_KEY";
const char* TS_HOST    = "api.thingspeak.com";

const unsigned long READ_INTERVAL = 20000UL; // 20 seconds, per spec
unsigned long lastReadTime = 0;

// Distance to report when the ultrasonic sensor times out (no echo).
// Treated as "far" so it never falsely triggers the buzzer.
const long NO_ECHO_DISTANCE_CM = 999;

void setup() {
    Serial.begin(9600);      // Serial Monitor, for your own debugging
  esp8266.begin(9600);     // must match the ESP8266's AT baud rate

  pinMode(PIR_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  dht.begin();

  Serial.println("Booting Smart Room Monitor...");
  connectWiFi();
}

void loop() {
    if (millis() - lastReadTime < READ_INTERVAL) {
    return; // not time yet -- keeps loop() non-blocking and simple
    }
  lastReadTime = millis();

  float temperature = dht.readTemperature();  // Celsius
  float humidity     = dht.readHumidity();     // %
  long  distance      = readDistanceCM();
  int   motion         = digitalRead(PIR_PIN);  // 1 = motion, 0 = none

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT read failed (NaN) -- skipping this cycle.");
    return; // don't upload garbage data, just wait for the next tick
  }

  // ---- Requirement 2: Local Alert ----
  bool alert = (motion == 1) || (distance < 20);
  digitalWrite(BUZZER_PIN, alert ? HIGH : LOW);

  Serial.print("Temp: ");      Serial.print(temperature);
  Serial.print(" C  Hum: ");   Serial.print(humidity);
  Serial.print(" %  Dist: ");  Serial.print(distance);
  Serial.print(" cm  Motion: "); Serial.print(motion);
  Serial.print("  Buzzer: ");  Serial.println(alert ? "ON" : "OFF");

  // ---- Requirement 3: Send to ThingSpeak ----
  sendToThingSpeak(temperature, humidity, distance, motion);
}

// Reads distance from the HC-SR04 ultrasonic sensor, in centimeters.
long readDistanceCM() {
    digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // 30ms timeout ~5m max range -- pulseIn returns 0 if it times out
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) {
    return NO_ECHO_DISTANCE_CM;
  }
  return duration * 0.034 / 2; // speed of sound, round trip
}

// ---------------- ESP8266 AT-COMMAND HELPERS ----------------

// Sends an AT command and waits up to `timeout` ms for `expected`
// to appear in the module's response. Returns true if it did.
bool sendATCommand(String cmd, String expected, unsigned long timeout) {
    if (cmd.length() > 0) {
    esp8266.println(cmd);
    }
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeout) {
    while (esp8266.available()) {
      response += (char)esp8266.read();
    }
    if (response.indexOf(expected) != -1) {
      return true;
    }
  }
  Serial.print("AT command timed out: "); Serial.println(cmd);
  return false;
}

void connectWiFi() {
    sendATCommand("AT+CWMODE=1", "OK", 2000); // station mode

  String cmd = "AT+CWJAP=\"" + String(WIFI_SSID) + "\",\"" +
               String(WIFI_PASS) + "\"";
  Serial.println("Connecting to WiFi...");
  if (sendATCommand(cmd, "OK", 10000)) {
    Serial.println("WiFi connected.");
  } else {
    Serial.println("WiFi connection failed -- check SSID/password/signal.");
  }
}

// Builds the ThingSpeak update URL and pushes it via a raw TCP
// connection + AT+CIPSEND (equivalent to an HTTP GET request).
void sendToThingSpeak(float temp, float hum, long dist, int motion) {
    String getStr = "GET /update?api_key=" + String(TS_API_KEY) +
                   "&field1=" + String(temp) +
                   "&field2=" + String(hum) +
                   "&field3=" + String(dist) +
                   "&field4=" + String(motion) +
                   "\r\n";

  if (!sendATCommand("AT+CIPSTART=\"TCP\",\"" + String(TS_HOST) + "\",80",
                          "OK", 5000)) {
    Serial.println("TCP connect to ThingSpeak failed.");
    return;
  }

  String cipSend = "AT+CIPSEND=" + String(getStr.length());
  esp8266.println(cipSend);
  delay(500); // give the module time to show the ">" send-prompt
  esp8266.print(getStr);

  if (sendATCommand("", "SEND OK", 5000)) {
    Serial.println("Data sent to ThingSpeak.");
  } else {
    Serial.println("Failed to send data to ThingSpeak.");
  }

  sendATCommand("AT+CIPCLOSE", "OK", 2000);
}
