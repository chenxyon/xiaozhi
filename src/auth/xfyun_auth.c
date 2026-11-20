/* 讯飞 WebAPI 通用鉴权实现 — Arduino 版
 *
 * 与 ESP-IDF 原版的差异：
 *   - PSA Crypto → mbedtls_md_hmac（Arduino core 内置 mbedtls 原生 API）
 *   - esp_log.h / freertos → Arduino 自带（底层还是 ESP-IDF，所以照样能用）
 *   - esp_err.h / ESP_OK / ESP_FAIL → Arduino 自带（同上）
 *   - SNTP 等待逻辑 → time() 在联网后自动同步，用 delay() 替代 vTaskDelay()
 *
 * 注意：本文件是 .c（纯 C），所以用 ESP_LOGI/E 而不是 Serial.printf
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "xfyun_auth.h"

static const char *TAG = "xfyun_auth";   /* 规则 4-8 */

/* NTP 同步判定阈值：time(NULL) 跳变超过 60 秒说明 SNTP 已覆盖编译默认值 */
#define XFYUN_NTP_JUMP_THRESHOLD 60

esp_err_t xfyun_wait_sntp(int max_wait_s)
{
    if (max_wait_s <= 0) max_wait_s = 20;

    /* 初始 time 可能有两种情况：
     *   A) 编译默认值（__DATE__ 构造的 2027 年）→ 等跳变或 COMPLETED
     *   B) 已经是正确时间（SNTP 之前同步过了）→ 直接通过，不用等
     */
    time_t initial = time(NULL);

    /* 1) 如果初始 time 已经合理（2024 年后），说明之前同步过了 */
    if (initial > XFYUN_NTP_JUMP_THRESHOLD + 1704067200L) {   /* 大于 2024-01-01 + 60s */
        ESP_LOGI(TAG, "✅ SNTP 之前已同步，初始 time=%lld 直接放行", (long long)initial);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SNTP 等待启动，初始 time=%lld（编译默认值，跳变 >%ds 才算真同步）",
             (long long)initial, XFYUN_NTP_JUMP_THRESHOLD);

    for (int i = 0; i < max_wait_s; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time_t now = time(NULL);
        long long jump = (long long)now - (long long)initial;
        sntp_sync_status_t st = sntp_get_sync_status();

        bool jumped = llabs(jump) > XFYUN_NTP_JUMP_THRESHOLD;
        bool st_done = (st == SNTP_SYNC_STATUS_COMPLETED);

        ESP_LOGI(TAG, "等待 SNTP… (%d/%d) time=%lld jump=%llds status=%d %s%s",
                 i + 1, max_wait_s, (long long)now, jump, (int)st,
                 jumped ? "⏩已跳变 " : "",
                 st_done ? "✅COMPLETED " : "");

        if (jumped || st_done) {
            ESP_LOGI(TAG, "✅ SNTP 已同步！time=%lld", (long long)now);
            return ESP_OK;
        }
    }

    /* 超时：打印诊断帮用户排查 */
    time_t final = time(NULL);
    long long jump = (long long)final - (long long)initial;
    ESP_LOGE(TAG, "SNTP %d s 未同步！initial=%lld final=%lld jump=%llds",
             max_wait_s, (long long)initial, (long long)final, jump);
    ESP_LOGE(TAG, "→ 可能原因：NTP 服务器不通、DNS 配置问题、configTime 未调用");
    ESP_LOGE(TAG, "→ 当前 time 是编译默认值（%lld），讯飞会判定 ts expired", (long long)final);
    return ESP_ERR_TIMEOUT;
}

void xfyun_gmt_date(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

void xfyun_url_encode(const char *in, char *out, size_t out_n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!in || !out || out_n == 0) return;
    for (size_t i = 0; in[i] && o + 4 < out_n; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* HMAC-SHA256 — mbedtls 原生 API（替代 ESP-IDF 专属的 PSA Crypto） */
esp_err_t xfyun_hmac_sha256(const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t out[32])
{
    int ret = mbedtls_md_hmac(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        key, key_len,
        msg, msg_len,
        out
    );
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

/* HMAC-SHA1 — RTASR 专用 */
static esp_err_t hmac_sha1(const uint8_t *key, size_t key_len,
                           const uint8_t *msg, size_t msg_len,
                           uint8_t out[20])
{
    int ret = mbedtls_md_hmac(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
        key, key_len,
        msg, msg_len,
        out
    );
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

/* MD5 → 32 字符小写 hex */
static esp_err_t md5_hex(const uint8_t *msg, size_t msg_len, char out[33])
{
    uint8_t digest[16];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_MD5), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, msg, msg_len);
    mbedtls_md_finish(&ctx, digest);
    mbedtls_md_free(&ctx);

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = hex[digest[i] >> 4];
        out[i*2+1] = hex[digest[i] & 0x0F];
    }
    out[32] = '\0';
    return ESP_OK;
}

esp_err_t xfyun_build_rtasr_url(const char *appid, const char *api_key,
                                char *out, size_t out_n)
{
    if (!appid || !api_key || !out || out_n == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. ts = 当前秒级时间戳 */
    time_t ts = time(NULL);
    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)ts);

    /* 2. baseString = MD5(appid + ts) */
    char concat[64];
    snprintf(concat, sizeof(concat), "%s%s", appid, ts_str);

    char md5hex[33];
    if (md5_hex((const uint8_t *)concat, strlen(concat), md5hex) != ESP_OK) {
        ESP_LOGE(TAG, "MD5 计算失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RTASR baseString(MD5)=%s", md5hex);

    /* 3. signa = base64(HMAC_SHA1(baseString, apiKey)) */
    uint8_t sha1out[20];
    if (hmac_sha1((const uint8_t *)api_key, strlen(api_key),
                  (const uint8_t *)md5hex, strlen(md5hex),
                  sha1out) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC-SHA1 计算失败");
        return ESP_FAIL;
    }

    char signa_b64[48] = {0};
    size_t signa_len = 0;
    mbedtls_base64_encode((unsigned char *)signa_b64, sizeof(signa_b64), &signa_len,
                          sha1out, sizeof(sha1out));

    /* 4. 对 appid / ts / signa 分别做 URL 编码再拼接
     * 官方文档要求：key 和 value 都要 urlencode
     * signa 是 base64，可能含 + / =，必须编码 */
    char appid_enc[64] = {0}, ts_enc[32] = {0}, signa_enc[96] = {0};
    xfyun_url_encode(appid, appid_enc, sizeof(appid_enc));
    xfyun_url_encode(ts_str, ts_enc, sizeof(ts_enc));
    xfyun_url_encode(signa_b64, signa_enc, sizeof(signa_enc));

    int n = snprintf(out, out_n,
                     "wss://rtasr.xfyun.cn/v1/ws?appid=%s&ts=%s&signa=%s",
                     appid_enc, ts_enc, signa_enc);
    if (n < 0 || (size_t)n >= out_n) {
        ESP_LOGE(TAG, "RTASR URL 缓冲不足");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "RTASR URL=%s", out);
    return ESP_OK;
}

esp_err_t xfyun_build_auth_url(const char *host, const char *path,
                               const char *api_key, const char *secret,
                               char *out, size_t out_n)
{
    if (!host || !path || !api_key || !secret || !out || out_n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!api_key[0] || !secret[0]) {
        ESP_LOGE(TAG, "API Key/Secret 未配置");
        return ESP_ERR_INVALID_STATE;
    }

    char date[64];
    xfyun_gmt_date(date, sizeof(date));

    char sign_src[256];
    snprintf(sign_src, sizeof(sign_src),
             "host: %s\ndate: %s\nGET %s HTTP/1.1", host, date, path);

    uint8_t hmac[32];
    if (xfyun_hmac_sha256((const uint8_t *)secret, strlen(secret),
                          (const uint8_t *)sign_src, strlen(sign_src),
                          hmac) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC-SHA256 计算失败");
        return ESP_FAIL;
    }

    char sig_b64[64] = {0};
    size_t sig_len = 0;
    mbedtls_base64_encode((unsigned char *)sig_b64, sizeof(sig_b64), &sig_len,
                          hmac, sizeof(hmac));

    char auth_origin[512];
    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", "
             "headers=\"host date request-line\", signature=\"%s\"",
             api_key, sig_b64);

    char auth_b64[768] = {0};
    size_t auth_len = 0;
    mbedtls_base64_encode((unsigned char *)auth_b64, sizeof(auth_b64), &auth_len,
                          (const unsigned char *)auth_origin,
                          strlen(auth_origin));

    char auth_enc[2048] = {0};
    char date_enc[128]  = {0};
    xfyun_url_encode(auth_b64, auth_enc, sizeof(auth_enc));
    xfyun_url_encode(date, date_enc, sizeof(date_enc));

    int n = snprintf(out, out_n,
                     "wss://%s%s?authorization=%s&date=%s&host=%s",
                     host, path, auth_enc, date_enc, host);
    if (n < 0 || (size_t)n >= out_n) {
        ESP_LOGE(TAG, "URL 缓冲不足（需 %d 字节，实有 %u）", n, (unsigned)out_n);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
