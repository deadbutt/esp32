/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"
#include "esp_err.h"
#include "esp_crypto_lock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "photo_cloud.h"
#include "photo_mqtt.h"
#include "photo_storage.h"
#include "photo_ui.h"

static photo_cloud_state_t s_cloud;
static photo_album_t s_album;
static TaskHandle_t s_cloud_sync_task;

#define PHOTO_FRAME_FW_MARKER "photo-frame-sync-20260813-0835-ble-ram-trim"
#define CLOUD_SYNC_TASK_STACK 24576

static void warmup_crypto_locks(void)
{
    esp_crypto_sha_aes_lock_acquire();
    esp_crypto_sha_aes_lock_release();
    esp_crypto_mpi_lock_acquire();
    esp_crypto_mpi_lock_release();
    ESP_LOGI("photo_main", "crypto locks warmed up for BLE security");
}

static void show_cloud_status(const char *fallback_status)
{
    if (lvgl_port_lock(-1)) {
        const char *status = s_cloud.status[0] != '\0' ? s_cloud.status : fallback_status;
        photo_ui_show_cloud(s_cloud.device_uid, status, s_cloud.setup_blocked ? -1 : s_cloud.media_count);
        lvgl_port_unlock();
    }
}

static bool frame_ready_for_cloud(void)
{
    return s_cloud.binding_state_known && s_cloud.bound;
}

static void cloud_sync_task(void *arg)
{
    (void)arg;
    bool mqtt_started = false;

    for (;;) {
        photo_mqtt_pause_slideshow();
        esp_err_t ret = photo_cloud_sync_once(&s_cloud, s_album.mounted ? &s_album : NULL);
        UBaseType_t remaining = uxTaskGetStackHighWaterMark(NULL);
        if (remaining < 1024) {
            ESP_LOGW("photo_main", "cloud sync task stack watermark is low: %u", (unsigned)remaining);
        }
        if (ret == ESP_OK) {
            if (!frame_ready_for_cloud()) {
                show_cloud_status(s_cloud.binding_state_known ? "相框未绑定" : "等待绑定确认");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            photo_mqtt_apply_playback(s_cloud.status);
            if (!mqtt_started) {
                photo_mqtt_start(&s_cloud, &s_album);
                mqtt_started = true;
            }
            break;
        }

        ESP_LOGW("photo_main", "cloud sync failed: %s", esp_err_to_name(ret));
        if (s_cloud.setup_blocked) {
            show_cloud_status("设备配置未完成，请联系售后");
            break;
        }
        if (s_cloud.provisioning_timed_out && !frame_ready_for_cloud()) {
            s_cloud.provisioning_timed_out = false;
            show_cloud_status("正在重新开启蓝牙配网");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        show_cloud_status(s_cloud.wifi_connected ? "云端同步失败，稍后自动重试" : "网络不可用，请检查 Wi-Fi");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }

    s_cloud_sync_task = NULL;
    vTaskDelete(NULL);
}

void app_main()
{
    ESP_LOGI("photo_main", "firmware marker: %s", PHOTO_FRAME_FW_MARKER);
    warmup_crypto_locks();

    photo_cloud_init_state(&s_cloud);
    photo_cloud_load_binding_state(&s_cloud);

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());

    if (lvgl_port_lock(-1)) {
        photo_ui_show_boot();
        lvgl_port_unlock();
    }

    esp_err_t storage_ret = photo_storage_mount(&s_album);
    if (storage_ret == ESP_OK && s_cloud.binding_state_known && s_cloud.bound && s_cloud.active_album_id > 0) {
        photo_storage_set_album_scope(&s_album, s_cloud.active_album_id);
        photo_storage_scan(&s_album);
    }

    photo_mqtt_attach(&s_cloud, &s_album);

    if (xTaskCreate(cloud_sync_task, "cloud_sync_start", CLOUD_SYNC_TASK_STACK, NULL, 5, &s_cloud_sync_task) != pdPASS) {
        ESP_LOGW("photo_main", "cloud sync task create failed");
        show_cloud_status("网络任务启动失败");
    }
}
