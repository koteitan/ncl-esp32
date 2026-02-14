#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Update.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "efontEnableJaMini.h"
#include "efont.h"
#include "../secrets.h"

#define VERSION "v1.2.2"
#define RELAY_HOST "yabu.me"
#define RELAY_PORT 443
#define RELAY_PATH "/"

WebSocketsClient webSocket;
WebServer server(80);

// --- アイコンキャッシュ（RGB565 32x32 = 2048 bytes each）---
#define ICON_SIZE 32
#define ICON_BYTES (ICON_SIZE * ICON_SIZE * 2)
#define META_CACHE_SIZE 100
#define ICON_BUF_COUNT 20  // 実際に画像をキャッシュする数（メモリ節約）

// アイコン画像バッファプール
uint16_t iconPool[ICON_BUF_COUNT][ICON_SIZE * ICON_SIZE];
bool iconPoolUsed[ICON_BUF_COUNT];

struct MetaEntry {
  String pubkey;
  String displayName;
  String pictureUrl;
  uint16_t color;
  int iconPoolIdx;    // -1 = 未取得, >=0 = iconPool index
  bool metaReceived;  // kind:0を受信済みか
  bool iconFailed;    // 画像取得失敗
};
MetaEntry metaCache[META_CACHE_SIZE];
int metaCacheCount = 0;

// --- 投稿データ ---
#define MAX_POSTS 5
struct Post {
  String content;
  String pubkey;
  unsigned long created_at;
};
Post posts[MAX_POSTS];
int postCount = 0;

bool connected = false;
bool relayStarted = false;
bool wifiReady = false;

// アイコンプールから空きを取得
int allocIconPool() {
  for (int i = 0; i < ICON_BUF_COUNT; i++) {
    if (!iconPoolUsed[i]) {
      iconPoolUsed[i] = true;
      return i;
    }
  }
  return -1; // 満杯
}

// pubkeyからカラーを生成
uint16_t pubkeyToColor(const String& pubkey) {
  if (pubkey.length() < 6) return WHITE;
  uint8_t r = strtol(pubkey.substring(0, 2).c_str(), NULL, 16);
  uint8_t g = strtol(pubkey.substring(2, 4).c_str(), NULL, 16);
  uint8_t b = strtol(pubkey.substring(4, 6).c_str(), NULL, 16);
  r = r / 2 + 128;
  g = g / 2 + 128;
  b = b / 2 + 128;
  return M5.Lcd.color565(r, g, b);
}

MetaEntry* findMeta(const String& pubkey) {
  for (int i = 0; i < metaCacheCount; i++) {
    if (metaCache[i].pubkey == pubkey) return &metaCache[i];
  }
  return NULL;
}

void addMeta(const String& pubkey, const String& displayName, const String& pictureUrl) {
  MetaEntry* existing = findMeta(pubkey);
  if (existing) {
    if (displayName.length() > 0) existing->displayName = displayName;
    if (pictureUrl.length() > 0) existing->pictureUrl = pictureUrl;
    existing->metaReceived = true;
    return;
  }
  int idx;
  if (metaCacheCount < META_CACHE_SIZE) {
    idx = metaCacheCount++;
  } else {
    // 最古を上書き（アイコンプール解放）
    idx = 0;
    if (metaCache[0].iconPoolIdx >= 0) {
      iconPoolUsed[metaCache[0].iconPoolIdx] = false;
    }
    for (int i = 0; i < META_CACHE_SIZE - 1; i++) metaCache[i] = metaCache[i + 1];
    idx = META_CACHE_SIZE - 1;
  }
  metaCache[idx].pubkey = pubkey;
  metaCache[idx].displayName = displayName;
  metaCache[idx].pictureUrl = pictureUrl;
  metaCache[idx].color = pubkeyToColor(pubkey);
  metaCache[idx].iconPoolIdx = -1;
  metaCache[idx].metaReceived = (displayName.length() > 0 || pictureUrl.length() > 0);
  metaCache[idx].iconFailed = false;
}

void requestMeta(const String& pubkey) {
  if (findMeta(pubkey)) return;
  addMeta(pubkey, "", "");
  
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  arr.add("REQ");
  arr.add("meta_" + pubkey.substring(0, 8));
  JsonObject filter = arr.add<JsonObject>();
  JsonArray kinds = filter["kinds"].to<JsonArray>();
  kinds.add(0);
  JsonArray authors = filter["authors"].to<JsonArray>();
  authors.add(pubkey);
  filter["limit"] = 1;
  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
}

// --- JPEG画像ダウンロード＆デコード ---
// Spriteに描画してからpixel読み出しで32x32に縮小
bool downloadIcon(MetaEntry* meta) {
  if (meta->pictureUrl.length() == 0) return false;
  if (meta->iconPoolIdx >= 0) return true; // 既に取得済み
  if (meta->iconFailed) return false;
  
  int poolIdx = allocIconPool();
  if (poolIdx < 0) return false; // プール満杯
  
  // HTTPS画像ダウンロード
  WiFiClientSecure client;
  client.setInsecure(); // 証明書検証スキップ
  HTTPClient http;
  
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  if (!http.begin(client, meta->pictureUrl)) {
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  
  int contentLen = http.getSize();
  if (contentLen > 100000 || contentLen == 0) {
    // 大きすぎる画像はスキップ（100KB制限）
    http.end();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  
  // 画像データをバッファに読み込み
  uint8_t* imgBuf = (uint8_t*)malloc(contentLen > 0 ? contentLen : 50000);
  if (!imgBuf) {
    http.end();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  
  WiFiClient* stream = http.getStreamPtr();
  int totalRead = 0;
  while (totalRead < (contentLen > 0 ? contentLen : 50000) && http.connected()) {
    int avail = stream->available();
    if (avail > 0) {
      int toRead = min(avail, (contentLen > 0 ? contentLen : 50000) - totalRead);
      int bytesRead = stream->readBytes(imgBuf + totalRead, toRead);
      totalRead += bytesRead;
    } else {
      delay(1);
    }
    yield();
  }
  http.end();
  
  if (totalRead < 100) {
    free(imgBuf);
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  
  // Spriteに描画（元サイズでデコード→32x32にリサンプル）
  // まず大きめのSpriteにJPEGデコード
  TFT_eSprite sprite = TFT_eSprite(&M5.Lcd);
  // 最大128x128でデコード（メモリ節約）
  int spriteSize = 128;
  sprite.createSprite(spriteSize, spriteSize);
  sprite.fillSprite(BLACK);
  
  // 先頭バイトでJPEG判定（PNGやその他はM5Core2 0.1.9では非対応）
  if (imgBuf[0] == 0xFF && imgBuf[1] == 0xD8) {
    sprite.drawJpg(imgBuf, totalRead, 0, 0, spriteSize, spriteSize);
  } else {
    // PNG/WebP等は未対応 → カラーブロック維持
    free(imgBuf);
    sprite.deleteSprite();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  free(imgBuf);
  
  // 128x128 → 32x32 にニアレストネイバーで縮小
  for (int y = 0; y < ICON_SIZE; y++) {
    for (int x = 0; x < ICON_SIZE; x++) {
      int srcX = x * spriteSize / ICON_SIZE;
      int srcY = y * spriteSize / ICON_SIZE;
      iconPool[poolIdx][y * ICON_SIZE + x] = sprite.readPixel(srcX, srcY);
    }
  }
  
  sprite.deleteSprite();
  meta->iconPoolIdx = poolIdx;
  return true;
}

// --- efont描画 ---
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

// --- UI描画 ---
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

String formatTime(unsigned long ts) {
  time_t t = (time_t)ts + 9 * 3600;
  struct tm* tm = gmtime(&t);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d",
    tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
  return String(buf);
}

void drawIcon(int x, int y, MetaEntry* meta, const String& name) {
  if (meta && meta->iconPoolIdx >= 0) {
    // キャッシュ済み画像を描画
    M5.Lcd.pushImage(x, y, ICON_SIZE, ICON_SIZE, iconPool[meta->iconPoolIdx]);
  } else {
    // カラーブロック＋頭文字
    uint16_t color = meta ? meta->color : WHITE;
    M5.Lcd.fillRoundRect(x, y, ICON_SIZE, ICON_SIZE, 4, color);
    if (name.length() > 0) {
      uint16_t firstChar;
      char* np = (char*)name.c_str();
      efontUFT8toUTF16(&firstChar, np);
      if (firstChar >= 0x20 && firstChar <= 0x7E) {
        M5.Lcd.setTextColor(BLACK, color);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setCursor(x + 8, y + 8);
        char fc[2] = {(char)firstChar, 0};
        M5.Lcd.print(fc);
      } else {
        byte font[32];
        memset(font, 0, 32);
        getefontData(font, firstChar);
        for (int row = 0; row < 16; row++) {
          for (int col = 0; col < 16; col++) {
            int byteIndex = row * 2 + col / 8;
            int bitIndex = 7 - (col % 8);
            if (font[byteIndex] & (1 << bitIndex))
              M5.Lcd.drawPixel(x + 8 + col, y + 8 + row, BLACK);
          }
        }
      }
    }
  }
}

void drawTimeline() {
  M5.Lcd.fillRect(0, 30, 320, 210, BLACK);
  int y = 32;
  for (int i = 0; i < postCount && i < MAX_POSTS; i++) {
    if (y > 225) break;
    if (i > 0) M5.Lcd.drawLine(5, y - 2, 315, y - 2, TFT_DARKGREY);
    
    Post& p = posts[i];
    MetaEntry* meta = findMeta(p.pubkey);
    String name = (meta && meta->displayName.length() > 0)
      ? meta->displayName : p.pubkey.substring(0, 8) + "...";
    String timeStr = formatTime(p.created_at);
    
    drawIcon(5, y, meta, name);
    efontDrawString(40, y, name, CYAN, 200, 1);
    
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(TFT_DARKGREY);
    M5.Lcd.setCursor(248, y + 4);
    M5.Lcd.print(timeStr.c_str());
    
    int h = efontDrawString(40, y + 17, p.content, WHITE, 275, 2);
    y += 17 + h + 4;
  }
}

void drawStatus(const char* msg) {
  M5.Lcd.fillRect(0, 222, 320, 18, BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(YELLOW);
  M5.Lcd.setCursor(5, 225);
  M5.Lcd.print(msg);
}

void drawIconStatusBar() {
  M5.Lcd.fillRect(0, 222, 320, 18, BLACK);
  if (metaCacheCount == 0) return;

  int barWidth = 320 / metaCacheCount;
  if (barWidth < 1) barWidth = 1;

  for (int i = 0; i < metaCacheCount; i++) {
    uint16_t color;
    if (metaCache[i].iconPoolIdx >= 0) {
      color = GREEN;
    } else if (metaCache[i].iconFailed) {
      color = TFT_DARKGREY;
    } else {
      color = BLACK;
    }
    int x = i * 320 / metaCacheCount;
    int w = (i + 1) * 320 / metaCacheCount - x;
    M5.Lcd.fillRect(x, 222, w, 18, color);
  }
}

// --- Nostr通信 ---
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
  drawIconStatusBar();
}

// アイコンダウンロードキュー
bool iconDownloadPending = false;

void handleEvent(uint8_t* payload, size_t length) {
  if (length > 4096) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  
  const char* type = doc[0];
  if (!type) return;
  
  if (strcmp(type, "EVENT") == 0) {
    int kind = doc[2]["kind"] | -1;
    
    if (kind == 0) {
      const char* pubkey = doc[2]["pubkey"];
      const char* content = doc[2]["content"];
      if (pubkey && content) {
        JsonDocument metaDoc;
        if (!deserializeJson(metaDoc, content)) {
          const char* dname = metaDoc["display_name"] | metaDoc["name"];
          const char* picture = metaDoc["picture"];
          addMeta(String(pubkey),
                  dname ? String(dname) : String(""),
                  picture ? String(picture) : String(""));
          iconDownloadPending = true;
          drawTimeline();
        }
      }
    } else if (kind == 1) {
      const char* content = doc[2]["content"];
      const char* pubkey = doc[2]["pubkey"];
      unsigned long created_at = doc[2]["created_at"] | 0;
      
      if (content && pubkey) {
        String post = String(content);
        post.replace("\n", " ");
        if (post.length() > 200) post = post.substring(0, 197) + "...";
        
        for (int i = MAX_POSTS - 1; i > 0; i--) posts[i] = posts[i - 1];
        posts[0].content = post;
        posts[0].pubkey = String(pubkey);
        posts[0].created_at = created_at;
        if (postCount < MAX_POSTS) postCount++;
        
        requestMeta(String(pubkey));
        drawTimeline();
      }
    }
  } else if (strcmp(type, "EOSE") == 0) {
    drawIconStatusBar();
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

// --- アイコンバックグラウンドダウンロード ---
// loopで1フレームに1枚ずつダウンロード
void processIconDownload() {
  if (!iconDownloadPending) return;

  // TLに表示されてるポストのアイコンを優先
  for (int i = 0; i < postCount && i < MAX_POSTS; i++) {
    MetaEntry* meta = findMeta(posts[i].pubkey);
    if (meta && meta->pictureUrl.length() > 0 && meta->iconPoolIdx < 0 && !meta->iconFailed) {
      drawIconStatusBar();
      if (downloadIcon(meta)) {
        drawTimeline();
      }
      drawIconStatusBar();
      return; // 1枚ずつ
    }
  }

  // metaCache全体から未取得のものを1枚ずつダウンロード
  for (int i = 0; i < metaCacheCount; i++) {
    MetaEntry* meta = &metaCache[i];
    if (meta->pictureUrl.length() > 0 && meta->iconPoolIdx < 0 && !meta->iconFailed) {
      drawIconStatusBar();
      if (downloadIcon(meta)) {
        drawTimeline();
      }
      drawIconStatusBar();
      return; // 1枚ずつ
    }
  }
  iconDownloadPending = false;
}

// --- Web OTA ---
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

// --- メインループ ---
void setup() {
  M5.begin();
  M5.Lcd.fillScreen(BLACK);
  
  // アイコンプール初期化
  memset(iconPoolUsed, 0, sizeof(iconPoolUsed));
  
  drawHeader();
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
  
  M5.Lcd.fillRect(0, 30, 320, 200, BLACK);
  efontDrawString(30, 50, String("Nostr Client for M5Stack"), WHITE, 280, 1);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(CYAN);
  M5.Lcd.setCursor(30, 100);
  M5.Lcd.print("IP: ");
  M5.Lcd.print(WiFi.localIP());
  efontDrawString(30, 150, String("タッチでリレーに接続"), GREEN, 280, 1);
  drawStatus("WiFi OK / OTA ready");
  
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
  if (wifiReady) server.handleClient();
  
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
    processIconDownload();
  }
  yield();
}
