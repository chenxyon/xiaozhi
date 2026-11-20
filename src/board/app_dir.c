/**
 * @file     app_dir.c
 * @brief    员工分机查询（954n2fwj.qwenwork.host 通讯录）
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "tts_hub.h"
#include "kws_hub.h"

#include "app_dir.h"

static const char *TAG = "app_dir";   /* 规则 4-8 */

#define DIR_URL     "https://954n2fwj.qwenwork.host/api/employees"
#define DIR_RX_CAP  (200 * 1024)      /* 通讯录约 47KB，留足余量 */
#define DIR_STACK   16384     /* cJSON 解析 47KB 通讯录 + TLS，栈要够，否则溢出重启 */
#define DIR_REPLY   512

static char  *s_rx;
static size_t s_rxlen;
static bool   s_ovf;

/* ---- 去掉常见疑问词，剩下的当姓名 ---- */
static void remove_sub(char *s, const char *sub)
{
    size_t L = strlen(sub);
    if (!L) return;
    char *p;
    while ((p = strstr(s, sub)) != NULL) {
        memmove(p, p + L, strlen(p + L) + 1);
    }
}

static void extract_name(const char *text, char *out, size_t n)
{
    char tmp[256];
    strncpy(tmp, text, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* 先截到"分机"之前，避免把问句后半段当名字 */
    char *p = strstr(tmp, "分机");
    if (p) *p = '\0';

    static const char *rm[] = {
        "请问", "帮我查一下", "帮我查", "查一下", "查询", "麻烦",
        "问一下", "是多少", "多少号", "多少", "号码", "电话",
        "分机", "一下", "的", "是", "查", "问",
        "？", "?", "。", "，", ",", "!", "！", " "
    };
    for (size_t i = 0; i < sizeof(rm) / sizeof(rm[0]); i++) remove_sub(tmp, rm[i]);

    /* 首尾去空白 */
    char *s = tmp;
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = '\0';

    strncpy(out, s, n - 1);
    out[n - 1] = '\0';
}

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data && e->data_len > 0) {
        if (s_rxlen + e->data_len < DIR_RX_CAP) {
            memcpy(s_rx + s_rxlen, e->data, e->data_len);
            s_rxlen += e->data_len;
            s_rx[s_rxlen] = '\0';
        } else s_ovf = true;
    }
    return ESP_OK;
}

/* 拉取通讯录并匹配姓名，成功填 reply（含三遍）返回 ESP_OK */
static esp_err_t dir_lookup(const char *name, char *reply, size_t rn)
{
    if (!s_rx) {
        s_rx = heap_caps_malloc(DIR_RX_CAP, MALLOC_CAP_SPIRAM);
        if (!s_rx) s_rx = malloc(DIR_RX_CAP);
        if (!s_rx) return ESP_ERR_NO_MEM;
    }
    s_rxlen = 0; s_ovf = false; s_rx[0] = '\0';

    esp_http_client_config_t cfg = {
        .url               = DIR_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_evt,
        .buffer_size       = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    esp_http_client_set_header(c, "Connection", "close");
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200 || s_rxlen == 0) {
        ESP_LOGE(TAG, "通讯录拉取失败 err=%s status=%d rx=%u",
                 esp_err_to_name(err), status, (unsigned)s_rxlen);
        return ESP_FAIL;
    }

    cJSON *j = cJSON_Parse(s_rx);
    if (!j) { ESP_LOGE(TAG, "通讯录 JSON 解析失败"); return ESP_FAIL; }
    cJSON *data = cJSON_GetObjectItem(j, "data");
    int n = cJSON_GetArraySize(data);

    const char *hit_name = NULL, *hit_ext = NULL;
    /* 先找完全相等，再找包含 */
    for (int pass = 0; pass < 2 && !hit_name; pass++) {
        for (int i = 0; i < n; i++) {
            cJSON *emp = cJSON_GetArrayItem(data, i);
            cJSON *jn = cJSON_GetObjectItem(emp, "name");
            if (!cJSON_IsString(jn) || !jn->valuestring[0]) continue;
            bool ok = (pass == 0) ? (strcmp(jn->valuestring, name) == 0)
                                  : (strstr(jn->valuestring, name) != NULL);
            if (ok) {
                hit_name = jn->valuestring;
                cJSON *je = cJSON_GetObjectItem(emp, "ext");
                hit_ext = (cJSON_IsString(je)) ? je->valuestring : "";
                break;
            }
        }
    }

    if (hit_name && hit_ext && hit_ext[0]) {
        /* 播三遍 */
        snprintf(reply, rn,
                 "%s，分机是 %s。%s，分机是 %s。%s，分机是 %s。",
                 hit_name, hit_ext, hit_name, hit_ext, hit_name, hit_ext);
        ESP_LOGI(TAG, "命中: %s -> 分机 %s", hit_name, hit_ext);
        cJSON_Delete(j);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "未找到姓名: %s", name);
    cJSON_Delete(j);
    return ESP_ERR_NOT_FOUND;
}

/* 等 TTS 播完 */
static void wait_tts_done(void)
{
    for (int i = 0; i < 30 && !tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 400 && tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
}

static void dir_task(void *arg)
{
    char *q = (char *)arg;
    char *reply = malloc(DIR_REPLY);

    if (!reply) {
        tts_hub_speak_async("系统内存不足。");
    } else {
        char name[128] = {0};
        extract_name(q, name, sizeof(name));

        esp_err_t r;
        if (!name[0]) r = ESP_ERR_NOT_FOUND;
        else          r = dir_lookup(name, reply, DIR_REPLY);

        vTaskDelay(pdMS_TO_TICKS(1200));   /* 让拉取通讯录的 TLS 释放后再开 TTS */

        if (r == ESP_OK) {
            tts_hub_speak_async(reply);
        } else {
            char msg[160];
            snprintf(msg, sizeof(msg), "没有找到%s的分机。", name[0] ? name : "该人员");
            tts_hub_speak_async(msg);
        }
        free(reply);
    }
    free(q);

    wait_tts_done();
    kws_hub_resume();
    vTaskDelete(NULL);
}

bool app_dir_matches(const char *text)
{
    return text && strstr(text, "分机") != NULL;
}

void app_dir_selftest(const char *name)
{
    char reply[DIR_REPLY];
    esp_err_t r = dir_lookup(name, reply, sizeof(reply));
    vTaskDelay(pdMS_TO_TICKS(400));
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "自检播报(三遍): %s", reply);
        tts_hub_speak_async(reply);
    } else {
        char m[160];
        snprintf(m, sizeof(m), "没有找到%s的分机。", name);
        tts_hub_speak_async(m);
    }
    wait_tts_done();
}

void app_dir_query_async(const char *question)
{
    char *c = question ? strdup(question) : NULL;
    if (!c) { kws_hub_resume(); return; }
    if (xTaskCreate(dir_task, "dir_q", DIR_STACK, c, 4, NULL) != pdPASS) {
        free(c);
        kws_hub_resume();
    }
}
