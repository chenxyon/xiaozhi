/**
 * @file     ArduinoCompat.h
 * @brief    ESP-IDF → Arduino 兼容层
 *
 * 作用：Arduino ESP32 Core 底层就是 ESP-IDF，driver/freertos/esp_log.h
 *       大部分都能直接用。本文件只补真正缺失的东西：
 *         1. psa/crypto.h  → mbedtls 原生 API（Arduino core 的 mbedtls
 *            没有启用 PSA Crypto，用 mbedtls_md_hmac 替代）
 *         2. esp_crt_bundle → WiFiClientSecure.setCACert()
 *         3. esp_websocket_client → WebSockets2_Generic 库（见 tts_xfyun_arduino.cpp）
 */
#ifndef ARDUINO_COMPAT_H
#define ARDUINO_COMPAT_H

#include <Arduino.h>

/* ---- Arduino ESP32 Core 自带，直接用 ---- */
/* #include "esp_err.h"        ← 自带 */
/* #include "esp_log.h"        ← 自带 */
/* #include "freertos/*.h"     ← 自带 */
/* #include "driver/gpio.h"   ← 自带 */
/* #include "driver/spi_master.h.h" ← 自带 */
/* #include "driver/i2s_std.h" ← 自带（取决于 core 版本，可能需要适配） */

/* ---- mbedtls（Arduino core 内置，直接用） ---- */
#include "mbedtls/base64.h"
#include "mbedtls/md.h"        /* mbedtls_md_hmac 替代 PSA Crypto */
#include "mbedtls/sha256.h"

#endif /* ARDUINO_COMPAT_H */
