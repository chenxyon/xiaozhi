/**
 * @file     secret_store.c
 * @brief    NVS 封装实现
 *
 * 修改：2026-11-20 新建（规则 4-1）
 */
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "secret_store.h"

static const char *TAG = "secret_store";

#define NVS_NAMESPACE  "xiaozhi"

esp_err_t secret_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除，重新初始化");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase 失败: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "NVS 就绪 (namespace=%s)", NVS_NAMESPACE);
    return ESP_OK;
}

esp_err_t secret_store_get_str(const char *key, char *out, size_t out_sz, size_t *got)
{
    if (!key || !out || out_sz == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 先查长度 */
    size_t len = 0;
    err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;   /* ESP_ERR_NOT_FOUND 等 */
    }

    if (len + 1 > out_sz) {
        /* 缓冲不够，截断 */
        len = out_sz - 1;
    }

    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str(%s) 失败: %s", key, esp_err_to_name(err));
        return err;
    }
    out[len] = '\0';
    if (got) *got = len;
    return ESP_OK;
}

esp_err_t secret_store_set_str(const char *key, const char *val)
{
    if (!key || !val) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(WRITE) 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) 失败: %s", key, esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已写入: %s", key);
    return ESP_OK;
}

esp_err_t secret_store_erase_all(void)
{
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase 失败: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_flash_init();
    return err;
}
