#include <M5Core2.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "efontEnableJaMini.h"
#include "efont.h"
#include "../secrets.h"

#define VERSION "v1.0.3"
#define RELAY_HOST "yabu.me"
#define RELAY_PORT 443
#define RELAY_PATH "/"

WebSocketsClient webSocket;
WebServer server(80);

#define MAX_POSTS 5
String posts[MAX_POSTS];
int postCount = 0;
bool connected = false;
bool relayStarted = false;
bool wifiReady = false;

// efontで1文字描画
int efontDrawChar(int x, int y, uint16_t utf16, uint16_t color) {
  if (utf16 >= 0x20 && utf16 <= 0x7E) {
    char c[2] = {(char)utf16, 0};
    M5.Lcd.setTextColor(color, BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(x, y);
    M5.Lcd.print(c);
    return 12;
  }
  
  byte font[32];
  memset(font, 0, 32);
  getefontData(font, utf16);
  
  for (int row = 0; row < 16; row++) {
    for (int col = 0; col < 16; col++) {
      int byteIndex = row * 2 + col / 8;
      int bitIndex = 7 - (col % 8);
      if (font[byteIndex] & (1 << bitIndex)) {
        M5.Lcd.drawPixel(x + col, y + row, color);
      }
    }
  }
  return 16;
}

int efontDrawString(int x, int y, const String& str, uint16_t color, int maxWidth, int maxLines) {
  int cx = x;
  int cy = y;
  int line = 0;
  char* p = (char*)str.c_str();
  
  while (*p && line < maxLines) {
    uint16_t utf16;
    p = efontUFT8toUTF16(&utf16, p);
    if (utf16 == 0) continue;
    
    int charWidth = (utf16 >= 0x20 && utf16 <= 0x7E) ? 12 : 16;
    
    if (cx + charWidth > x + maxWidth) {
      cx = x;
      cy += 17;
      line++;
      if (line >= maxLines) break;
    }
    
    efontDrawChar(cx, cy, utf16, color);
    cx += charWidth;
  }
  
  return (line + 1) * 17;
}

void drawHeader() {
  M5.Lcd.fillRect(0, 0, 320, 28, TFT_NAVY);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 6);
  M5.Lcd.print("ncl-core2");
  
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(CYAN);
  M5.Lcd.setCursor(150, 10);
  M5.Lcd.print(VERSION);
  
  M5.Lcd.setCursor(230, 10);
  if (connected) {
    M5.Lcd.setTextColor(GREEN);
    M5.Lcd.print("yabu.me");
  } else {
    M5.Lcd.setTextColor(RED);
    M5.Lcd.print("offline");
  }
}

void drawTimeline() {
  M5.Lcd.fillRect(0, 30, 320, 210, BLACK);
  int y = 32;
  for (int i = 0; i < postCount && i < MAX_POSTS; i++) {
    if (i > 0) M5.Lcd.drawLine(5, y - 2, 315, y - 2, TFT_DARKGREY);
    int h = efontDrawString(5, y, posts[i], WHITE, 310, 2);
    y += h + 4;
    if (y > 230) break;
  }
}

void drawStatus(const char* msg) {
  M5.Lcd.fillRect(0, 222, 320, 18, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(YELLOW);
  M5.Lcd.setCursor(5, 225);
  M5.Lcd.print(msg);
}

void sendSubscribe() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  arr.add("REQ");
  arr.add("tl");
  JsonObject filter = arr.add<JsonObject>();
  JsonArray kinds = filter["kinds"].to<JsonArray>();
  kinds.add(1);
  filter["limit"] = MAX_POSTS;
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  drawStatus("Subscribed to timeline...");
}

void handleEvent(uint8_t* payload, size_t length) {
  if (length > 4096) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  
  if (doc[0] == "EVENT") {
    const char* content = doc[2]["content"];
    if (content) {
      String post = String(content);
      post.replace("\n", " ");
      if (post.length() > 200) post = post.substring(0, 197) + "...";
      for (int i = MAX_POSTS - 1; i > 0; i--) posts[i] = posts[i - 1];
      posts[0] = post;
      if (postCount < MAX_POSTS) postCount++;
      drawTimeline();
    }
  } else if (doc[0] == "EOSE") {
    drawStatus("Timeline loaded!");
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      connected = false;
      drawHeader();
      drawStatus("Disconnected, reconnecting...");
      break;
    case WStype_CONNECTED:
      connected = true;
      drawHeader();
      sendSubscribe();
      break;
    case WStype_TEXT:
      handleEvent(payload, length);
      break;
    default: break;
  }
}

void setupWebOTA() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<h1>ncl-core2 OTA</h1>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'>"
      "<input type='submit' value='Upload'>"
      "</form>");
  });
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    delay(500);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      webSocket.disconnect();
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.setTextSize(2);
      M5.Lcd.setTextColor(YELLOW);
      M5.Lcd.setCursor(40, 100);
      M5.Lcd.print("OTA Updating...");
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
      int pct = (Update.progress() * 100) / Update.size();
      M5.Lcd.fillRect(40, 140, 240, 20, BLACK);
      M5.Lcd.setCursor(40, 140);
      M5.Lcd.printf("%d%%", pct);
      M5.Lcd.fillRect(40, 170, pct * 240 / 100, 10, GREEN);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
      M5.Lcd.setCursor(40, 200);
      M5.Lcd.setTextColor(GREEN);
      M5.Lcd.print("Done! Rebooting...");
    }
  });
  server.begin();
}

void setup() {
  M5.begin();
  M5.Lcd.fillScreen(BLACK);
  drawHeader();
  
  // Phase 1: WiFi接続
  drawStatus("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    efontDrawString(30, 80, String("WiFi接続失敗"), RED, 280, 1);
    drawStatus("Reboot to retry");
    return;
  }
  
  wifiReady = true;
  setupWebOTA();
  
  // Phase 2: IP表示 & タッチ待ち
  M5.Lcd.fillRect(0, 30, 320, 200, BLACK);
  efontDrawString(30, 50, String("Nostr Client for M5Stack"), WHITE, 280, 1);
  
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(CYAN);
  M5.Lcd.setCursor(30, 100);
  M5.Lcd.print("IP: ");
  M5.Lcd.print(WiFi.localIP());
  
  efontDrawString(30, 150, String("タッチでリレーに接続"), GREEN, 280, 1);
  drawStatus("WiFi OK / OTA ready");
  
  // タッチパネル初期化時の誤検知を防ぐ：一度離されるまで待つ
  delay(500);
  M5.update();
  while (M5.Touch.ispressed()) {
    M5.update();
    server.handleClient();
    delay(50);
  }
  delay(200);
}

void loop() {
  M5.update();
  
  if (wifiReady) {
    server.handleClient();
  }
  
  if (!relayStarted) {
    if (wifiReady && M5.Touch.ispressed()) {
      relayStarted = true;
      M5.Lcd.fillRect(0, 30, 320, 200, BLACK);
      drawHeader();
      drawStatus("Connecting relay...");
      webSocket.beginSSL(RELAY_HOST, RELAY_PORT, RELAY_PATH);
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
    }
  } else {
    webSocket.loop();
  }
  yield();
}
