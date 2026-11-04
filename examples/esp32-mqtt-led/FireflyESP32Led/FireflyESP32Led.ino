#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <cstring>
#include "secrets.h"

constexpr uint8_t LED_GPIO = 26;
constexpr char TOPIC_SET[]   = "firefly/esp32/led/set";
constexpr char TOPIC_STATE[] = "firefly/esp32/led/state";
constexpr unsigned long STATE_INTERVAL_MS = 15000;

WiFiClient net;
PubSubClient mqtt(net);
bool ledOn = false;
unsigned long lastRetry = 0;
bool wifiWasConnected = false;
unsigned long lastStateAttempt = 0;

void publishCurrentState() {
  if (!mqtt.connected()) return;
  lastStateAttempt = millis();  // A failed send must not spin/retry every loop.
  // GPIO readback, not proof that a physical load switched. Never retained.
  if (!mqtt.publish(TOPIC_STATE, digitalRead(LED_GPIO) == HIGH ? "ON" : "OFF", false)) {
    Serial.println("MQTT state publish failed");
  }
}

void setLed(bool on, bool publishState = true) {
  ledOn = on;
  digitalWrite(LED_GPIO, on ? HIGH : LOW);
  Serial.printf("LED: %s\n", on ? "ON" : "OFF");
  if (publishState) publishCurrentState();
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, TOPIC_SET) != 0) return;
  if (length == 2 && memcmp(payload, "ON", 2) == 0) {
    setLed(true);
  } else if (length == 3 && memcmp(payload, "OFF", 3) == 0) {
    setLed(false);
  } else {
    Serial.println("Ignored invalid MQTT command");
  }
}

void setup() {
  // GPIO is driven LOW before becoming an output.
  digitalWrite(LED_GPIO, LOW);
  pinMode(LED_GPIO, OUTPUT);
  Serial.begin(115200);
  delay(250);
  Serial.println("Firefly ESP32 MQTT LED starting");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);
}

void loop() {
  bool wifiNowConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiNowConnected != wifiWasConnected) {
    wifiWasConnected = wifiNowConnected;
    if (wifiNowConnected) {
      Serial.print("Wi-Fi connected: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("Wi-Fi disconnected");
    }
  }
  if (!wifiNowConnected) {
    // Demonstration fail-safe: LED goes OFF when network is gone.
    if (ledOn) setLed(false, false);
    delay(20);
    return;
  }
  if (!mqtt.connected()) {
    if (ledOn) setLed(false, false);
    if (millis() - lastRetry >= 3000 || lastRetry == 0) {
      lastRetry = millis();
      char clientId[32];
      snprintf(clientId, sizeof(clientId), "ff-esp32-%012llX", (unsigned long long)ESP.getEfuseMac());
      Serial.printf("MQTT connecting to %s:%u...\n", MQTT_HOST, MQTT_PORT);
      if (mqtt.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD)) {
        if (mqtt.subscribe(TOPIC_SET, 1)) {
          Serial.println("MQTT connected; subscribed");
          // Announce current GPIO state only after a live connection.
          publishCurrentState();
        } else {
          Serial.println("MQTT subscribe failed");
          mqtt.disconnect();
        }
      } else {
        Serial.printf("MQTT error state=%d\n", mqtt.state());
      }
    }
    return;
  }
  mqtt.loop();
  if (mqtt.connected() && millis() - lastStateAttempt >= STATE_INTERVAL_MS) {
    publishCurrentState();
  }
}
