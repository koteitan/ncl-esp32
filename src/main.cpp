#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>

// WiFi設定（secrets.hに移動予定）
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

// nostr relay
const char* relay_host = "relay.damus.io";
const int relay_port = 443;
const bool use_ssl = true;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("🐾 ncl-esp32 starting...");
  
  // WiFi接続
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n✅ WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // TODO: nostr relay接続とイベント処理
  delay(1000);
}
