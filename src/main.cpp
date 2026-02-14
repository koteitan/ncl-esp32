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
#include <rom/miniz.h>
#include <setjmp.h>
#include <mbedtls/base64.h>
#include <webp/decode.h>
// libjpeg for progressive JPEG (1/8 scale decode)
// Must match libjpeg's boolean=int to ensure struct size consistency
#define HAVE_BOOLEAN
typedef int jpeg_boolean;  // avoid conflict with Arduino's boolean
// Temporarily redefine boolean for jpeglib.h inclusion
#pragma push_macro("boolean")
#undef boolean
#define boolean int
extern "C" {
#include "jpeglib.h"
}
#pragma pop_macro("boolean")

#define VERSION "v1.5.0"
// RELAY_HOST, RELAY_PORT, RELAY_PATH は secrets.h で定義
// 例: #define RELAY_HOST "yabu.me"
//     #define RELAY_PORT 443
//     #define RELAY_PATH "/"

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

// --- WebP デコーダ (libwebp, スケーリング対応) ---
bool decodeWebpToSprite(const uint8_t* data, int dataLen, TFT_eSprite& sprite, int spriteSize) {
  int w, h;
  if (!WebPGetInfo(data, dataLen, &w, &h)) {
    Serial.println("[WEBP] GetInfo failed");
    return false;
  }
  Serial.printf("[WEBP] %dx%d, heap=%d\n", w, h, ESP.getFreeHeap());

  // スケーリングデコード: spriteSize以下に縮小
  WebPDecoderConfig config;
  if (!WebPInitDecoderConfig(&config)) { Serial.println("[WEBP] init config failed"); return false; }

  config.options.use_scaling = 1;
  // 直接ICON_SIZE(32x32)にスケーリング → ヒープ節約
  int targetSize = ICON_SIZE; // 32
  int scaledW = (w <= targetSize) ? w : targetSize;
  int scaledH = (h <= targetSize) ? h : targetSize;
  // アスペクト比維持
  if (w > h) { scaledH = h * targetSize / w; }
  else { scaledW = w * targetSize / h; }
  if (scaledW < 1) scaledW = 1;
  if (scaledH < 1) scaledH = 1;
  config.options.scaled_width = scaledW;
  config.options.scaled_height = scaledH;
  config.output.colorspace = MODE_RGB;

  size_t outSize = scaledW * scaledH * 3;
  Serial.printf("[WEBP] scaling to %dx%d (%d bytes), psram=%d\n", scaledW, scaledH, outSize, ESP.getFreePsram());

  VP8StatusCode status = WebPDecode(data, dataLen, &config);
  if (status != VP8_STATUS_OK) { Serial.printf("[WEBP] decode failed: %d\n", status); WebPFreeDecBuffer(&config.output); return false; }

  uint8_t* rgb = config.output.u.RGBA.rgba;
  int stride = config.output.u.RGBA.stride;
  for (int y = 0; y < scaledH; y++) {
    for (int x = 0; x < scaledW; x++) {
      int idx = y * stride + x * 3;
      sprite.drawPixel(x, y, sprite.color565(rgb[idx], rgb[idx+1], rgb[idx+2]));
    }
    yield();
  }

  WebPFreeDecBuffer(&config.output);
  Serial.println("[WEBP] decode OK");
  return true;
}

// --- PNG デコーダ (自作) ---
// imgBuf: PNGファイルデータ, imgLen: データ長
// sprite: デコード先Sprite (createSprite済み, spriteSize x spriteSize)
// 成功時true
bool decodePngToSprite(const uint8_t* imgBuf, int imgLen, TFT_eSprite& sprite, int spriteSize) {
  // PNGシグネチャ確認
  const uint8_t pngSig[] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
  if (imgLen < 33 || memcmp(imgBuf, pngSig, 8) != 0) { Serial.println("[PNG] bad signature"); return false; }

  // IHDRチャンク解析 (offset 8)
  int pos = 8;
  // chunk length (4) + "IHDR" (4) + data (13) + CRC (4)
  uint32_t ihdrLen = (imgBuf[pos]<<24)|(imgBuf[pos+1]<<16)|(imgBuf[pos+2]<<8)|imgBuf[pos+3];
  if (ihdrLen != 13 || memcmp(imgBuf+pos+4, "IHDR", 4) != 0) { Serial.printf("[PNG] bad IHDR len=%d\n", ihdrLen); return false; }
  pos += 8; // skip length + type
  uint32_t pngW = (imgBuf[pos]<<24)|(imgBuf[pos+1]<<16)|(imgBuf[pos+2]<<8)|imgBuf[pos+3]; pos+=4;
  uint32_t pngH = (imgBuf[pos]<<24)|(imgBuf[pos+1]<<16)|(imgBuf[pos+2]<<8)|imgBuf[pos+3]; pos+=4;
  uint8_t bitDepth = imgBuf[pos++];
  uint8_t colorType = imgBuf[pos++];
  uint8_t compression = imgBuf[pos++];
  uint8_t filter = imgBuf[pos++];
  uint8_t interlace = imgBuf[pos++];
  pos += 4; // CRC

  Serial.printf("[PNG] %dx%d depth=%d color=%d interlace=%d\n", pngW, pngH, bitDepth, colorType, interlace);
  if ((bitDepth != 8 && bitDepth != 4 && bitDepth != 2 && bitDepth != 1) || interlace != 0) { Serial.println("[PNG] unsupported depth/interlace"); return false; }
  if (colorType != 0 && colorType != 2 && colorType != 3 && colorType != 6) { Serial.printf("[PNG] unsupported colorType=%d\n", colorType); return false; }
  // メモリ制限: 256x256以上はスキップ（ESP32ヒープ保護）
  // ストリーミングデコードなので大きな画像もOK（メモリは行バッファ+32KBリングのみ）
  // ただし極端に大きい場合はDL時間がかかるので制限
  if (pngW > 4096 || pngH > 4096) { Serial.printf("[PNG] too large %dx%d, skip\n", pngW, pngH); return false; }

  // channels: ピクセルあたりのバイト数（8bit時）。パレットとグレースケールは1
  int channels = (colorType == 0) ? 1 : (colorType == 2) ? 3 : (colorType == 3) ? 1 : 4;

  // PLTEチャンク読み取り（colorType=3用）
  uint8_t palette[256][3];
  int paletteCount = 0;
  if (colorType == 3) {
    int pltPos = pos;
    while (pltPos + 12 <= imgLen) {
      uint32_t cLen = (imgBuf[pltPos]<<24)|(imgBuf[pltPos+1]<<16)|(imgBuf[pltPos+2]<<8)|imgBuf[pltPos+3];
      if (memcmp(imgBuf+pltPos+4, "PLTE", 4) == 0) {
        paletteCount = cLen / 3;
        if (paletteCount > 256) paletteCount = 256;
        for (int i = 0; i < paletteCount; i++) {
          palette[i][0] = imgBuf[pltPos+8+i*3];
          palette[i][1] = imgBuf[pltPos+8+i*3+1];
          palette[i][2] = imgBuf[pltPos+8+i*3+2];
        }
        Serial.printf("[PNG] PLTE: %d colors\n", paletteCount);
        break;
      } else if (memcmp(imgBuf+pltPos+4, "IDAT", 4) == 0) break;
      pltPos += 12 + cLen;
    }
    if (paletteCount == 0) { Serial.println("[PNG] no PLTE for indexed"); return false; }
  }

  // IDATチャンクを結合
  // まずIDATの合計サイズを計算（切り詰められたPNG対応）
  size_t totalIdat = 0;
  int scanPos = pos;
  while (scanPos + 8 <= imgLen) {
    uint32_t cLen = (imgBuf[scanPos]<<24)|(imgBuf[scanPos+1]<<16)|(imgBuf[scanPos+2]<<8)|imgBuf[scanPos+3];
    if (memcmp(imgBuf+scanPos+4, "IDAT", 4) == 0) {
      // 実際に読めるデータ量（ファイル末尾で切れてる場合）
      size_t avail = (scanPos + 8 + cLen <= (uint32_t)imgLen) ? cLen : (imgLen - scanPos - 8);
      totalIdat += avail;
    }
    else if (memcmp(imgBuf+scanPos+4, "IEND", 4) == 0) break;
    scanPos += 12 + cLen;
    if (scanPos > imgLen) break; // 切り詰めPNG: ループ終了（エラーにしない）
  }
  Serial.printf("[PNG] totalIdat=%d\n", totalIdat);
  if (totalIdat == 0) { Serial.println("[PNG] no IDAT"); return false; }

  // IDATデータを結合バッファにコピー（切り詰め対応）
  uint8_t* idatBuf = (uint8_t*)malloc(totalIdat);
  if (!idatBuf) return false;
  size_t idatOff = 0;
  scanPos = pos;
  while (scanPos + 8 <= imgLen) {
    uint32_t cLen = (imgBuf[scanPos]<<24)|(imgBuf[scanPos+1]<<16)|(imgBuf[scanPos+2]<<8)|imgBuf[scanPos+3];
    if (memcmp(imgBuf+scanPos+4, "IDAT", 4) == 0) {
      size_t avail = (scanPos + 8 + cLen <= (uint32_t)imgLen) ? cLen : (imgLen - scanPos - 8);
      memcpy(idatBuf + idatOff, imgBuf + scanPos + 8, avail);
      idatOff += avail;
    } else if (memcmp(imgBuf+scanPos+4, "IEND", 4) == 0) break;
    scanPos += 12 + cLen;
    if (scanPos > imgLen) break;
  }

  if (totalIdat < 6) { Serial.println("[PNG] IDAT too small"); free(idatBuf); return false; }

  // デコード先: 各行 = filterByte(1) + ceil(width * channels * bitDepth / 8)
  size_t pixelBits = pngW * channels * bitDepth;
  size_t rowBytes = 1 + (pixelBits + 7) / 8;
  int stride = (int)(rowBytes - 1); // filterByte除く1行のバイト数
  int bpp = max(1, (channels * bitDepth + 7) / 8);

  Serial.printf("[PNG] rowBytes=%d, stride=%d, idatSize=%d, heap=%d, psram=%d\n",
    rowBytes, stride, totalIdat, ESP.getFreeHeap(), ESP.getFreePsram());

  // ストリーミングデコード: リングバッファ + 2行分のみ確保
  // tinflリングバッファ: 2のべき乗サイズ（32KB）
  const size_t RING_SIZE = 32768;
  uint8_t* ringBuf = (uint8_t*)malloc(RING_SIZE);
  uint8_t* curRow = (uint8_t*)malloc(stride);
  uint8_t* prevRowBuf = (uint8_t*)calloc(stride, 1);
  tinfl_decompressor* decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  if (!ringBuf || !curRow || !prevRowBuf || !decomp) {
    Serial.println("[PNG] stream alloc failed");
    free(ringBuf); free(curRow); free(prevRowBuf); free(decomp); free(idatBuf);
    return false;
  }
  tinfl_init(decomp);

  const uint8_t* zlibData = idatBuf;
  size_t zlibLen = totalIdat;
  size_t inPos = 0;
  size_t ringPos = 0;       // リングバッファ内の書き込み位置
  size_t rowBufPos = 0;     // 現在の行バッファ内の位置（filterByte含む）
  uint32_t curY = 0;        // 現在の行番号
  uint8_t filterType = 0;
  bool decodeOk = true;
  tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;

  // 行データ一時バッファ（filterByte + stride）
  uint8_t* rowRaw = (uint8_t*)malloc(rowBytes);
  if (!rowRaw) { free(ringBuf); free(curRow); free(prevRowBuf); free(decomp); free(idatBuf); return false; }

  while (curY < pngH && status != TINFL_STATUS_DONE) {
    // tinflでリングバッファにデコード
    size_t inBytes = zlibLen - inPos;
    size_t outBytes = RING_SIZE - (ringPos & (RING_SIZE - 1));
    int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT;
    if (inPos + inBytes >= zlibLen) flags &= ~TINFL_FLAG_HAS_MORE_INPUT;
    // リングバッファモード（NON_WRAPPINGフラグなし）
    uint8_t* outStart = ringBuf + (ringPos & (RING_SIZE - 1));
    status = tinfl_decompress(decomp, zlibData + inPos, &inBytes, ringBuf, outStart, &outBytes, flags);
    inPos += inBytes;

    // デコードされたバイトを行バッファに詰める
    size_t bytesAvail = outBytes;
    uint8_t* src = outStart;
    while (bytesAvail > 0 && curY < pngH) {
      size_t need = rowBytes - rowBufPos;
      size_t take = (bytesAvail < need) ? bytesAvail : need;
      memcpy(rowRaw + rowBufPos, src, take);
      rowBufPos += take;
      src += take;
      bytesAvail -= take;

      if (rowBufPos >= rowBytes) {
        // 1行分揃った → フィルタ復元
        filterType = rowRaw[0];
        uint8_t* rawData = rowRaw + 1;
        for (int i = 0; i < stride; i++) {
          uint8_t raw = rawData[i];
          uint8_t a = (i >= bpp) ? curRow[i - bpp] : 0;
          uint8_t b = prevRowBuf[i];
          uint8_t c = (i >= bpp) ? prevRowBuf[i - bpp] : 0;
          switch (filterType) {
            case 0: curRow[i] = raw; break;
            case 1: curRow[i] = raw + a; break;
            case 2: curRow[i] = raw + b; break;
            case 3: curRow[i] = raw + ((a + b) >> 1); break;
            case 4: {
              int p = (int)a + b - c;
              int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
              curRow[i] = raw + ((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c);
              break;
            }
            default: curRow[i] = raw; break;
          }
        }

        // Spriteに描画（ニアレストネイバー縮小）
        int destY = curY * spriteSize / pngH;
        bool shouldDraw = (curY == pngH - 1) || ((int)((curY+1) * spriteSize / pngH) != destY);
        if (shouldDraw && destY < spriteSize) {
          for (int dx = 0; dx < spriteSize; dx++) {
            uint32_t srcX = dx * pngW / spriteSize;
            uint8_t r, g, b_val;
            if (colorType == 3) {
              uint8_t idx;
              if (bitDepth == 8) { idx = curRow[srcX]; }
              else {
                int pixelsPerByte = 8 / bitDepth;
                int byteIdx = srcX / pixelsPerByte;
                int bitOffset = (pixelsPerByte - 1 - (srcX % pixelsPerByte)) * bitDepth;
                idx = (curRow[byteIdx] >> bitOffset) & ((1 << bitDepth) - 1);
              }
              if (idx < paletteCount) { r = palette[idx][0]; g = palette[idx][1]; b_val = palette[idx][2]; }
              else { r = g = b_val = 0; }
            } else if (colorType == 0) { r = g = b_val = curRow[srcX]; }
            else { r = curRow[srcX*channels]; g = curRow[srcX*channels+1]; b_val = curRow[srcX*channels+2]; }
            sprite.drawPixel(dx, destY, sprite.color565(r, g, b_val));
          }
        }

        memcpy(prevRowBuf, curRow, stride);
        curY++;
        rowBufPos = 0;
        if (curY % 100 == 0) yield();
      }
    }
    ringPos += outBytes;

    if (status < 0) { Serial.printf("[PNG] inflate error: %d\n", status); decodeOk = false; break; }
  }

  free(rowRaw);
  free(decomp);
  free(idatBuf);
  free(ringBuf);
  free(curRow);
  free(prevRowBuf);

  Serial.printf("[PNG] streaming decode: %d/%d rows, status=%d\n", curY, pngH, status);
  if (!decodeOk && curY == 0) { Serial.println("[PNG] decode failed, no rows"); return false; }
  if (curY < pngH) { Serial.printf("[PNG] partial decode: %d/%d rows (truncated PNG?)\n", curY, pngH); }
  return true; // 部分デコードでもOK（切り詰めPNG対応）
}

// --- JPEG画像ダウンロード＆デコード ---
// Spriteに描画してからpixel読み出しで32x32に縮小
bool downloadIcon(MetaEntry* meta) {
  if (meta->pictureUrl.length() == 0) return false;
  if (meta->iconPoolIdx >= 0) return true; // 既に取得済み
  if (meta->iconFailed) return false;

  int poolIdx = allocIconPool();
  if (poolIdx < 0) return false; // プール満杯

  // data: URI対応 (Base64埋め込み画像)
  Serial.printf("[ICON] url prefix: %.20s\n", meta->pictureUrl.c_str());
  if (meta->pictureUrl.startsWith("data:image/")) {
    int b64Start = meta->pictureUrl.indexOf("base64,");
    if (b64Start < 0) { meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false; }
    b64Start += 7; // "base64," の後
    String b64 = meta->pictureUrl.substring(b64Start);
    // Base64デコード（ESP32のmbedtlsを使用）
    size_t b64Len = b64.length();
    size_t outLen = (b64Len * 3) / 4 + 4;
    uint8_t* imgBuf = (uint8_t*)malloc(outLen);
    if (!imgBuf) { meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false; }
    size_t actualLen = 0;
    int ret = mbedtls_base64_decode(imgBuf, outLen, &actualLen, (const uint8_t*)b64.c_str(), b64Len);
    if (ret != 0) {
      Serial.printf("[ICON] base64 decode failed: %d\n", ret);
      free(imgBuf); meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false;
    }
    Serial.printf("[ICON] data: URI decoded %d bytes\n", actualLen);
    // 以降のデコード処理へ（imgBuf, totalRead を設定してジャンプ）
    int totalRead = actualLen;
    // Sprite作成
    TFT_eSprite sprite = TFT_eSprite(&M5.Lcd);
    int spriteSize = 128;
    sprite.createSprite(spriteSize, spriteSize);
    sprite.fillSprite(BLACK);
    Serial.printf("[ICON] format: %02X %02X, size: %d, url: data:...\n", imgBuf[0], imgBuf[1], totalRead);
    // PNG判定
    if (imgBuf[0] == 0x89 && imgBuf[1] == 0x50) {
      if (!decodePngToSprite(imgBuf, totalRead, sprite, spriteSize)) {
        free(imgBuf); sprite.deleteSprite(); meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false;
      }
    } else {
      // 非PNG data: URI（JPEG等）→ drawJpg試行
      sprite.drawJpg(imgBuf, totalRead, 0, 0, spriteSize, spriteSize);
    }
    free(imgBuf);
    // iconPoolへコピー
    for (int y = 0; y < ICON_SIZE; y++) {
      for (int x = 0; x < ICON_SIZE; x++) {
        int srcX = x * spriteSize / ICON_SIZE;
        int srcY = y * spriteSize / ICON_SIZE;
        uint16_t px = sprite.readPixel(srcX, srcY);
        iconPool[poolIdx][y * ICON_SIZE + x] = (px >> 8) | (px << 8);
      }
    }
    sprite.deleteSprite();
    meta->iconPoolIdx = poolIdx;
    return true;
  }

  // HTTPS画像ダウンロード
  WiFiClientSecure client;
  client.setInsecure(); // 証明書検証スキップ
  HTTPClient http;

  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, meta->pictureUrl)) {
    Serial.printf("[ICON] http.begin failed: %s\n", meta->pictureUrl.c_str());
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("[ICON] HTTP %d: %s\n", httpCode, meta->pictureUrl.c_str());
    http.end();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }

  int contentLen = http.getSize();
  Serial.printf("[ICON] contentLen: %d\n", contentLen);
  if (contentLen > 500000) {
    Serial.printf("[ICON] size skip: %d\n", contentLen);
    http.end();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }

  // 画像データをバッファに読み込み
  uint8_t* imgBuf = NULL;
  int totalRead = 0;

  if (contentLen > 0) {
    // Content-Length既知: 直接ストリーム読み取り
    if (contentLen > 500000) {
      Serial.printf("[ICON] size skip: %d\n", contentLen);
      http.end(); meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false;
    }
    imgBuf = (uint8_t*)malloc(contentLen);
    if (!imgBuf) { http.end(); meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false; }
    WiFiClient* stream = http.getStreamPtr();
    unsigned long dlStart = millis();
    while (totalRead < contentLen && http.connected()) {
      int avail = stream->available();
      if (avail > 0) {
        int toRead = min(avail, contentLen - totalRead);
        totalRead += stream->readBytes(imgBuf + totalRead, toRead);
      } else { delay(1); }
      if (millis() - dlStart > 10000) break;
      yield();
    }
  } else {
    // Content-Length不明(chunked): writeToStreamでHTTPClientにデコードさせる
    // カスタムStreamクラスでバイナリデータをバッファに受け取る
    class BufStream : public Stream {
    public:
      uint8_t* buf; int pos; int cap;
      BufStream(uint8_t* b, int c) : buf(b), pos(0), cap(c) {}
      size_t write(uint8_t b) override { if (pos < cap) { buf[pos++] = b; return 1; } return 0; }
      size_t write(const uint8_t* d, size_t len) override {
        int toWrite = min((int)len, cap - pos);
        memcpy(buf + pos, d, toWrite); pos += toWrite; return toWrite;
      }
      int available() override { return 0; }
      int read() override { return -1; }
      int peek() override { return -1; }
      void flush() override {}
    };
    int bufSize = 500000;
    imgBuf = (uint8_t*)malloc(bufSize);
    if (!imgBuf) { http.end(); meta->iconFailed = true; iconPoolUsed[poolIdx] = false; return false; }
    BufStream bs(imgBuf, bufSize);
    http.writeToStream(&bs);
    totalRead = bs.pos;
    Serial.printf("[ICON] chunked body: %d bytes\n", totalRead);
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

  // 先頭バイトで画像形式判定
  Serial.printf("[ICON] format: %02X %02X, size: %d, url: %s\n", imgBuf[0], imgBuf[1], totalRead, meta->pictureUrl.c_str());
  if (imgBuf[0] == 0xFF && imgBuf[1] == 0xD8) {
    // JPEG
    Serial.println("[ICON] JPEG decode start");
    // JPEGの元サイズとタイプを取得
    int jpgW = 0, jpgH = 0;
    bool progressiveJpeg = false;
    for (int ji = 0; ji < totalRead - 9; ji++) {
      if (imgBuf[ji] == 0xFF && (imgBuf[ji+1] == 0xC0 || imgBuf[ji+1] == 0xC1 || imgBuf[ji+1] == 0xC2)) {
        if (imgBuf[ji+1] == 0xC2) progressiveJpeg = true;
        jpgH = (imgBuf[ji+5] << 8) | imgBuf[ji+6];
        jpgW = (imgBuf[ji+7] << 8) | imgBuf[ji+8];
        break;
      }
    }
    Serial.printf("[JPEG] original: %dx%d%s\n", jpgW, jpgH, progressiveJpeg ? " (progressive)" : "");
    if (progressiveJpeg && (jpgW > 1000 || jpgH > 1000)) {
      // 大きすぎるプログレッシブJPEGはDCT係数バッファ(数MB)がPSRAMに入りきらない
      Serial.printf("[JPEG] progressive %dx%d too large, skipping\n", jpgW, jpgH);
      free(imgBuf);
      sprite.deleteSprite();
      meta->iconFailed = true;
      iconPoolUsed[poolIdx] = false;
      return false;
    }
    if (progressiveJpeg) {
      Serial.printf("[JPEG] progressive - using libjpeg 1/8 scale, heap=%d psram=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());
      // libjpeg: メモリソースからデコード（1/8スケール）
      struct jpeg_decompress_struct cinfo;
      struct jpeg_error_mgr jerr;
      // setjmpでエラーをキャッチ（libjpegデフォルトはabort→リブート）
      struct JpegErrorMgr { struct jpeg_error_mgr pub; jmp_buf jmpBuf; };
      JpegErrorMgr errMgr;
      cinfo.err = jpeg_std_error(&errMgr.pub);
      errMgr.pub.error_exit = [](j_common_ptr ci) {
        JpegErrorMgr* myerr = (JpegErrorMgr*)ci->err;
        char buf[JMSG_LENGTH_MAX];
        ci->err->format_message(ci, buf);
        Serial.printf("[JPEG] libjpeg error: %s\n", buf);
        longjmp(myerr->jmpBuf, 1);
      };
      if (setjmp(errMgr.jmpBuf)) {
        // エラー発生時
        Serial.println("[JPEG] libjpeg decode failed, skipping");
        jpeg_destroy_decompress(&cinfo);
        free(imgBuf);
        sprite.deleteSprite();
        meta->iconFailed = true;
        iconPoolUsed[poolIdx] = false;
        return false;
      }
      jpeg_create_decompress(&cinfo);
      jpeg_mem_src(&cinfo, imgBuf, totalRead);
      jpeg_read_header(&cinfo, TRUE);
      // 1/8スケールデコード（800x800→100x100等）
      cinfo.scale_num = 1;
      cinfo.scale_denom = 8;
      cinfo.out_color_space = JCS_RGB;
      jpeg_start_decompress(&cinfo);
      int outW = cinfo.output_width;
      int outH = cinfo.output_height;
      int outCh = cinfo.output_components;
      Serial.printf("[JPEG] libjpeg scaled: %dx%d ch=%d\n", outW, outH, outCh);
      // スキャンライン読み取り → Spriteに直接描画
      sprite.deleteSprite();
      spriteSize = min(max(outW, outH), 128);
      sprite.createSprite(spriteSize, spriteSize);
      sprite.fillSprite(BLACK);
      uint8_t* rowBuf = (uint8_t*)malloc(outW * outCh);
      if (rowBuf) {
        int dy = 0;
        while (cinfo.output_scanline < cinfo.output_height) {
          JSAMPROW row = rowBuf;
          jpeg_read_scanlines(&cinfo, &row, 1);
          if (dy < spriteSize) {
            for (int dx = 0; dx < min(outW, spriteSize); dx++) {
              int idx = dx * outCh;
              sprite.drawPixel(dx, dy, sprite.color565(rowBuf[idx], rowBuf[idx+1], rowBuf[idx+2]));
            }
          }
          dy++;
          if (dy % 20 == 0) yield();
        }
        free(rowBuf);
      }
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      free(imgBuf);
      // リサンプル→iconPoolへ（以降の共通処理に流す）
      goto resample_to_icon;
    }
    // スケール選択: デコード後がspriteSize以下になる最大スケール
    jpeg_div_eSprite_t jpgScale = JPEG_DIV_ESPRITE_NONE;
    int maxDim = max(jpgW, jpgH);
    if (maxDim > spriteSize * 4) jpgScale = JPEG_DIV_ESPRITE_8;
    else if (maxDim > spriteSize * 2) jpgScale = JPEG_DIV_ESPRITE_4;
    else if (maxDim > spriteSize) jpgScale = JPEG_DIV_ESPRITE_2;
    // スケール後のサイズでSpriteを再作成
    int scaledDim = maxDim;
    if (jpgScale == JPEG_DIV_ESPRITE_8) scaledDim = maxDim / 8;
    else if (jpgScale == JPEG_DIV_ESPRITE_4) scaledDim = maxDim / 4;
    else if (jpgScale == JPEG_DIV_ESPRITE_2) scaledDim = maxDim / 2;
    if (scaledDim > spriteSize) scaledDim = spriteSize; // クリップ
    // Spriteをデコードサイズに合わせて再作成
    int jpgDecW = jpgW, jpgDecH = jpgH;
    if (jpgScale == JPEG_DIV_ESPRITE_8) { jpgDecW /= 8; jpgDecH /= 8; }
    else if (jpgScale == JPEG_DIV_ESPRITE_4) { jpgDecW /= 4; jpgDecH /= 4; }
    else if (jpgScale == JPEG_DIV_ESPRITE_2) { jpgDecW /= 2; jpgDecH /= 2; }
    if (jpgDecW < 1) jpgDecW = 1;
    if (jpgDecH < 1) jpgDecH = 1;
    int jpgSprSize = max(jpgDecW, jpgDecH);
    if (jpgSprSize > spriteSize) jpgSprSize = spriteSize;
    sprite.deleteSprite();
    spriteSize = jpgSprSize;
    sprite.createSprite(spriteSize, spriteSize);
    sprite.fillSprite(BLACK);
    Serial.printf("[JPEG] scale=%d, spriteSize=%d (%dx%d)\n", jpgScale, spriteSize, jpgDecW, jpgDecH);
    sprite.drawJpg(imgBuf, totalRead, 0, 0, spriteSize, spriteSize, 0, 0, jpgScale);
    Serial.println("[ICON] JPEG decode done");
  } else if (imgBuf[0] == 0x89 && imgBuf[1] == 0x50) {
    // PNG
    Serial.println("[ICON] PNG decode start");
    if (!decodePngToSprite(imgBuf, totalRead, sprite, spriteSize)) {
      Serial.println("[ICON] PNG decode FAILED");
      free(imgBuf);
      sprite.deleteSprite();
      meta->iconFailed = true;
      iconPoolUsed[poolIdx] = false;
      return false;
    }
  } else if (imgBuf[0] == 0x52 && imgBuf[1] == 0x49) {
    // WebP (RIFF header) - 直接ICON_SIZEにスケーリングデコード
    Serial.println("[ICON] WebP decode start");
    sprite.deleteSprite();
    spriteSize = ICON_SIZE; // 32x32
    sprite.createSprite(spriteSize, spriteSize);
    sprite.fillSprite(BLACK);
    if (!decodeWebpToSprite(imgBuf, totalRead, sprite, spriteSize)) {
      Serial.println("[ICON] WebP decode FAILED");
      free(imgBuf);
      sprite.deleteSprite();
      meta->iconFailed = true;
      iconPoolUsed[poolIdx] = false;
      return false;
    }
  } else {
    // 未対応形式 → カラーブロック維持
    free(imgBuf);
    sprite.deleteSprite();
    meta->iconFailed = true;
    iconPoolUsed[poolIdx] = false;
    return false;
  }
  free(imgBuf);

resample_to_icon:
  // spriteSize x spriteSize → 32x32 にニアレストネイバーで縮小
  // pushImageはバイトスワップされたRGB565を期待する場合があるのでswap
  for (int y = 0; y < ICON_SIZE; y++) {
    for (int x = 0; x < ICON_SIZE; x++) {
      int srcX = x * spriteSize / ICON_SIZE;
      int srcY = y * spriteSize / ICON_SIZE;
      uint16_t px = sprite.readPixel(srcX, srcY);
      iconPool[poolIdx][y * ICON_SIZE + x] = (px >> 8) | (px << 8); // バイトスワップ
    }
  }
  sprite.deleteSprite();
  meta->iconPoolIdx = poolIdx;
  return true;
}

// --- efont描画 ---
int efontDrawChar(int x, int y, uint16_t utf16, uint16_t color) {
  byte font[32];
  memset(font, 0, 32);
  getefontData(font, utf16);

  // ASCII(半角)は8px幅、CJKは16px幅
  bool halfWidth = (utf16 >= 0x20 && utf16 <= 0x7E);
  int charWidth = halfWidth ? 8 : 16;

  for (int row = 0; row < 16; row++) {
    for (int col = 0; col < charWidth; col++) {
      int byteIndex = row * 2 + col / 8;
      int bitIndex = 7 - (col % 8);
      if (font[byteIndex] & (1 << bitIndex)) {
        M5.Lcd.drawPixel(x + col, y + row, color);
      }
    }
  }
  return charWidth;
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
  M5.Lcd.print("noscli-core2");
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(CYAN);
  M5.Lcd.setCursor(160, 10);
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
  M5.Lcd.fillRect(0, 239, 320, 1, BLACK);
  if (metaCacheCount == 0) return;

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
    if (w < 1) w = 1;
    M5.Lcd.fillRect(x, 239, w, 1, color);
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
  if (length > 16384) return; // data:URI入りkind:0対応（旧4096→16384）
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

  // metaCache全体から未取得のものを1枚ずつダウンロード（新しい方から＝スタック型）
  for (int i = metaCacheCount - 1; i >= 0; i--) {
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
  efontDrawString(30, 50, String("noscli-core2"), WHITE, 280, 1);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(CYAN);
  M5.Lcd.setCursor(30, 100);
  M5.Lcd.print("IP: ");
  M5.Lcd.print(WiFi.localIP());
  efontDrawString(30, 150, String("タッチでリレーに接続"), GREEN, 280, 1);
  drawStatus("WiFi OK / OTA ready");

  // テストコード削除済み

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
