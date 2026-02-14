# Libraries

## libjpeg (IJG libjpeg 9f)
- Source: http://www.ijg.org/
- Decode-only (no encoder files)
- Used for **progressive JPEG** 1/8 scale decoding
- Baseline JPEG is handled by M5Stack's built-in `drawJpg`
- ESP32 porting notes:
  - `jconfig.h`: minimal config for ESP32
  - `jmorecfg.h`: modified boolean handling (Arduino defines `boolean` as `bool`, libjpeg needs `int`)
  - `jmemnobs.c`: no-backing-store memory manager (uses standard malloc)

## libwebp (Google libwebp)
- Source: https://github.com/nicestrudoc/libwebp (decode-only subset)
- Used for **WebP** image decoding with direct 32x32 scaling
- `HAVE_CONFIG_H` defined to disable SSE/NEON auto-detection on ESP32
- Encoder and platform-specific SIMD files excluded via `library.json` srcFilter
