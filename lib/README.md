# Libraries

## 画像デコーダ一覧

| 形式 | デコーダ | 自作/外部 | 備考 |
|---|---|---|---|
| JPEG (baseline) | M5Stack `drawJpg` | 外部 (M5内蔵) | SOFマーカー解析で1/1〜1/8自動スケール |
| JPEG (progressive) | IJG libjpeg 9f | 外部 (ESP32にポーティング) | 1/8スケールデコード、≤1000px |
| PNG | tinfl (inflate) + 自作パーサ | **自作** | IHDRパース、IDAT結合、フィルタ復元(5種)、パレット対応、ストリーミングデコード |
| WebP | Google libwebp 1.5.0 | 外部 (ESP32にポーティング) | `use_scaling` で直接32x32デコード |

## Image format support summary

| Format | Method | Max size | Notes |
|---|---|---|---|
| JPEG baseline | M5 `drawJpg` + SOF scale | any | 1/1~1/8 auto-scale |
| JPEG progressive ≤1000px | libjpeg 1/8 scale | 1000x1000 | ~100KB memory |
| JPEG progressive >1000px | skip | - | DCT coeff buffer ~3MB, PSRAM fragmentation |
| PNG | Custom decoder (tinfl) | rawSize ≤4MB | Full filter reconstruction, nearest-neighbor downscale |
| PNG (large) | skip | >4MB rawSize | 2000x2000 RGBA = 16MB, exceeds PSRAM |
| WebP | libwebp direct 32x32 | any | use_scaling, PSRAM allocator |

## Chunked transfer handling
- Content-Length known: direct stream read with 10s timeout
- Content-Length unknown (chunked): `http.writeToStream()` + custom `BufStream` class
  - `getString()` is NOT binary-safe (truncates at null bytes)
  - `getStream()`/`getStreamPtr()` returns raw TCP (no chunked decoding)
  - `writeToStream()` handles chunked decoding and writes binary-safe to buffer

---

## 画像デコーダで踏んだ問題と対処法

### tinfl構造体のスタック溢れ
- **問題**: tinfl_decompressorが約11KBでESP32のタスクスタック(8KB)を超える
- **対処**: `malloc()` でヒープに確保

### RGB565バイトスワップ
- **問題**: `readPixel()` と `pushImage()` でエンディアンが逆。色がめちゃくちゃになる
- **対処**: iconPool保存時に `(px >> 8) | (px << 8)` でスワップ

### libjpeg boolean型不一致
- **問題**: Arduino `boolean` = `bool` (1B)、libjpeg `boolean` = `int` (4B)。構造体のオフセットがずれてabort
- **対処**: `#pragma push_macro("boolean")` でjpeglib.hインクルード時だけ `boolean=int` に再定義

### libjpeg abort/exit
- **問題**: デコードエラー時に `exit()` → ESP32再起動
- **対処**: setjmp/longjmpでエラーハンドラを差し替え

### 巨大画像のメモリ不足 (PNG)
- **問題**: 2000x2000 RGBA = rawSize 16MB、PSRAM 4MBでも無理
- **対処**: ストリーミングデコーダ。tinflリングバッファ(32KB) + 行バッファ2本。行単位でinflate→フィルタ復元→Spriteに描画→バッファ上書き。67KBで16MB画像をデコード

### chunked transfer encoding
- **問題**: `getString()` がぬるバイトで切断、`getStream()` がchunkedデコードしない
- **対処**: カスタム `BufStream` クラス + `writeToStream()` でバイナリ安全にダウンロード

---

## libjpeg (IJG libjpeg 9f)
- Source: http://www.ijg.org/
- Decode-only (no encoder files)
- Used for **progressive JPEG** 1/8 scale decoding
- Baseline JPEG is handled by M5Stack's built-in `drawJpg`
- ESP32 porting notes:
  - `jconfig.h`: minimal config for ESP32
  - `jmorecfg.h`: modified boolean handling (Arduino defines `boolean` as `bool` 1byte, libjpeg needs `int` 4bytes — struct size mismatch causes abort)
  - `jmemnobs.c`: uses PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for large DCT coefficient buffers
  - `main.cpp`: `#pragma push_macro("boolean")` to temporarily redefine boolean=int during jpeglib.h inclusion
  - setjmp/longjmp error handler to prevent abort() on decode failure (ESP32 reboots on abort)

## libwebp (Google libwebp 1.5.0)
- Source: https://github.com/nicestrudoc/libwebp (decode-only subset)
- Used for **WebP** image decoding with direct 32x32 scaling via `WebPDecoderConfig.options.use_scaling`
- Supports VP8, VP8L (lossless), VP8X (extended format with ICCP profiles)
- ESP32 porting notes:
  - `HAVE_CONFIG_H` defined to disable SSE/NEON auto-detection on ESP32
  - Encoder and platform-specific SIMD files excluded via `library.json` srcFilter
  - `utils.c`: uses PSRAM via `heap_caps_malloc/calloc(MALLOC_CAP_SPIRAM)` for internal decode buffers (1105x1105 images need several MB)
