#include "photo_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "photo_ui.h"

#define PHOTO_MQTT_CMD_QUEUE_LEN 8
#define PHOTO_MQTT_CMD_PAYLOAD_MAX 8192
#define PHOTO_MQTT_CMD_TASK_STACK 8192
#define PHOTO_MQTT_SYNC_TASK_STACK 16384
#define PHOTO_MQTT_STATE_HEARTBEAT_TASK_STACK 6144
#define PHOTO_MQTT_STATE_HEARTBEAT_MS 60000
#define PHOTO_MQTT_RECENT_COMMAND_IDS 8

static const char *PHOTO_MQTT_TAG = "photo_mqtt";
static photo_cloud_state_t *s_state;
static photo_album_t *s_album;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_slideshow_task;
static TaskHandle_t s_command_task;
static TaskHandle_t s_sync_task;
static TaskHandle_t s_state_heartbeat_task;
static QueueHandle_t s_command_queue;
static volatile bool s_sync_requested;
static volatile bool s_sync_running;
static bool s_mqtt_first_connect_handled;
static volatile bool s_mqtt_connected;
static volatile bool s_slideshow_stop_requested;
static StaticTask_t s_sync_task_buffer;
static StackType_t s_sync_task_stack[PHOTO_MQTT_SYNC_TASK_STACK] __attribute__((aligned(16)));
static size_t s_slideshow_index;
static char s_cmd_topic[96];
static char s_state_topic[96];
static char s_event_topic[96];
static char s_recent_command_ids[PHOTO_MQTT_RECENT_COMMAND_IDS][96];
static size_t s_recent_command_id_index;

typedef struct {
    size_t len;
    char *payload;
} photo_mqtt_command_t;

static bool media_id_from_cloud_path(const char *path, int *media_id);
static bool can_use_cloud_photos(void);
static bool has_manual_photos(void);
static void persist_cloud_state(void);
static void sync_catalog_now(void);
static void handle_command_payload(const char *payload, size_t payload_len);
static void enqueue_command_payload(const char *payload, size_t payload_len);
static bool ensure_sync_catalog_task(void);
static void ensure_command_worker(void);

static void publish_topic_json(const char *topic, cJSON *root)
{
    if (s_client == NULL || topic == NULL || root == NULL) {
        return;
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return;
    }
    esp_mqtt_client_publish(s_client, topic, json, 0, 1, 0);
    cJSON_free(json);
}

static void publish_state(const char *reason)
{
    if (s_state == NULL) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return;
    }
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "device_uid", s_state->device_uid);
    if (s_state->frame_id[0] != '\0') {
        cJSON_AddStringToObject(root, "frame_id", s_state->frame_id);
    }
    cJSON_AddNumberToObject(root, "config_version", s_state->config_version);
    cJSON_AddNumberToObject(root, "content_version", s_state->content_version);
    if (reason != NULL) {
        cJSON_AddStringToObject(root, "reason", reason);
    }
    cJSON_AddStringToObject(data, "status", s_state->status);
    cJSON_AddNumberToObject(data, "brightness", s_state->brightness);
    cJSON_AddNumberToObject(data, "slideshow_interval_sec", s_state->slideshow_interval_sec);
    cJSON_AddStringToObject(data, "play_mode", s_state->play_mode);
    cJSON_AddNumberToObject(data, "current_media_id", s_state->current_media_id);
    cJSON_AddNumberToObject(data, "active_album_id", s_state->active_album_id);
    cJSON_AddNumberToObject(data, "media_count", s_state->media_count);
    cJSON_AddNumberToObject(data, "downloaded_count", s_state->downloaded_count);
    if (s_album != NULL) {
        cJSON_AddNumberToObject(data, "local_count", (double)s_album->count);
    }
    cJSON_AddItemToObject(root, "data", data);
    publish_topic_json(s_state_topic, root);
    cJSON_Delete(root);
}

static void state_heartbeat_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PHOTO_MQTT_STATE_HEARTBEAT_MS));
        if (s_mqtt_connected && s_client != NULL && s_state != NULL && s_state->wifi_connected && s_state->bound) {
            publish_state("heartbeat");
        }
    }
}

static void ensure_state_heartbeat_task(void)
{
    if (s_state_heartbeat_task != NULL) {
        return;
    }
    if (xTaskCreate(state_heartbeat_task, "mqtt_state_hb",
                    PHOTO_MQTT_STATE_HEARTBEAT_TASK_STACK, NULL, 4,
                    &s_state_heartbeat_task) != pdPASS) {
        s_state_heartbeat_task = NULL;
        ESP_LOGW(PHOTO_MQTT_TAG, "state heartbeat task create failed");
    }
}

static void publish_event(const char *type, cJSON *data)
{
    if (s_state == NULL || type == NULL) {
        cJSON_Delete(data);
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));
    cJSON_AddStringToObject(root, "device_uid", s_state->device_uid);
    if (data != NULL) {
        cJSON_AddItemToObject(root, "data", data);
    }
    publish_topic_json(s_event_topic, root);
    cJSON_Delete(root);
}

static void show_local_photo(const char *path, const char *status)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (lvgl_port_lock(100)) {
        photo_ui_show_local_photo(path, status);
        if (s_state != NULL) {
            photo_ui_set_brightness(s_state->brightness);
        }
        lvgl_port_unlock();
    }
}

static bool current_photo_path(char *path, size_t size)
{
    if (s_state == NULL || s_album == NULL) {
        return false;
    }

    if (s_state->current_media_id > 0 && can_use_cloud_photos()) {
        photo_storage_lock();
        for (size_t i = 0; i < s_album->count; i++) {
            int path_media_id = 0;
            if (media_id_from_cloud_path(s_album->paths[i], &path_media_id) &&
                path_media_id == s_state->current_media_id) {
                snprintf(path, size, "%s", s_album->paths[i]);
                photo_storage_unlock();
                return true;
            }
        }
        photo_storage_unlock();
    }

    return false;
}

static const char *photo_basename(const char *path)
{
    const char *name = path == NULL ? NULL : strrchr(path, '/');
    return name == NULL ? path : name + 1;
}

static bool is_cloud_photo_path(const char *path)
{
    return photo_storage_is_cloud_path(path);
}

static bool has_cloud_photos(void)
{
    if (s_album == NULL) {
        return false;
    }
    photo_storage_lock();
    for (size_t i = 0; i < s_album->count; i++) {
        if (is_cloud_photo_path(s_album->paths[i])) {
            photo_storage_unlock();
            return true;
        }
    }
    photo_storage_unlock();
    return false;
}

static bool can_use_cloud_photos(void)
{
    return s_state != NULL && s_state->binding_state_known && s_state->bound;
}

static bool has_manual_photos(void)
{
    if (s_album == NULL) {
        return false;
    }
    photo_storage_lock();
    for (size_t i = 0; i < s_album->count; i++) {
        if (!is_cloud_photo_path(s_album->paths[i])) {
            photo_storage_unlock();
            return true;
        }
    }
    photo_storage_unlock();
    return false;
}

static bool media_id_from_cloud_path(const char *path, int *media_id)
{
    return photo_storage_cloud_media_id_from_path(path, media_id);
}

static int command_int(cJSON *root, cJSON *data, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsNumber(item) && data != NULL) {
        item = cJSON_GetObjectItem(data, name);
    }
    return cJSON_IsNumber(item) ? item->valueint : -1;
}

static bool command_is_stale(cJSON *root, cJSON *data)
{
    if (s_state == NULL) {
        return true;
    }
    int config_version = command_int(root, data, "config_version");
    int content_version = command_int(root, data, "content_version");
    bool has_newer_config = config_version > s_state->config_version;
    bool has_newer_content = content_version > s_state->content_version;
    if (has_newer_config || has_newer_content) {
        return false;
    }
    bool stale_config = config_version >= 0 && config_version < s_state->config_version;
    bool stale_content = content_version >= 0 && content_version < s_state->content_version;
    if (stale_config || stale_content) {
        ESP_LOGW(PHOTO_MQTT_TAG,
                 "ignore stale command config_version=%d current=%d content_version=%d current=%d",
                 config_version, s_state->config_version, content_version, s_state->content_version);
        return true;
    }
    return false;
}

static void remember_command_versions(cJSON *root, cJSON *data)
{
    if (s_state == NULL) {
        return;
    }
    int config_version = command_int(root, data, "config_version");
    int content_version = command_int(root, data, "content_version");
    if (config_version >= 0 && config_version > s_state->config_version) {
        s_state->config_version = config_version;
    }
    if (content_version >= 0 && content_version > s_state->content_version) {
        s_state->content_version = content_version;
    }
}

static bool remember_command_id(cJSON *root)
{
    cJSON *message_id = cJSON_GetObjectItem(root, "message_id");
    if (!cJSON_IsString(message_id) || message_id->valuestring[0] == '\0') {
        return true;
    }
    for (size_t i = 0; i < PHOTO_MQTT_RECENT_COMMAND_IDS; i++) {
        if (strcmp(s_recent_command_ids[i], message_id->valuestring) == 0) {
            ESP_LOGI(PHOTO_MQTT_TAG, "ignore duplicate command id=%s", message_id->valuestring);
            return false;
        }
    }
    snprintf(s_recent_command_ids[s_recent_command_id_index],
             sizeof(s_recent_command_ids[s_recent_command_id_index]), "%s", message_id->valuestring);
    s_recent_command_id_index = (s_recent_command_id_index + 1) % PHOTO_MQTT_RECENT_COMMAND_IDS;
    return true;
}

static void persist_cloud_state(void)
{
    if (s_state != NULL && s_state->binding_state_known) {
        photo_cloud_save_cached_state(s_state);
    }
}

static bool first_displayable_photo_index(size_t *out_index)
{
    if (s_album == NULL || out_index == NULL) {
        return false;
    }
    bool prefer_cloud = can_use_cloud_photos() && has_cloud_photos();
    for (size_t i = 0; i < s_album->count; i++) {
        bool is_cloud = is_cloud_photo_path(s_album->paths[i]);
        if ((prefer_cloud && is_cloud) || (!prefer_cloud && !is_cloud)) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static bool album_index_from_media_id(int media_id, size_t *out_index)
{
    if (s_album == NULL || media_id <= 0 || out_index == NULL) {
        return false;
    }
    for (size_t i = 0; i < s_album->count; i++) {
        int path_media_id = 0;
        if (media_id_from_cloud_path(s_album->paths[i], &path_media_id) && path_media_id == media_id) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static void log_slideshow_state(const char *reason)
{
    const char *mode = s_state != NULL ? s_state->play_mode : "(null)";
    int current = s_state != NULL ? s_state->current_media_id : 0;
    int album_id = s_state != NULL ? s_state->active_album_id : 0;
    size_t count = s_album != NULL ? s_album->count : 0;
    ESP_LOGI(PHOTO_MQTT_TAG, "%s mode=%s current_media_id=%d active_album_id=%d slideshow_index=%u album_count=%u",
             reason != NULL ? reason : "slideshow_state",
             mode, current, album_id, (unsigned)s_slideshow_index, (unsigned)count);
}

static void update_current_media_from_path(const char *path)
{
    int media_id = 0;
    if (s_state != NULL && media_id_from_cloud_path(path, &media_id)) {
        s_state->current_media_id = media_id;
    }
}

static void align_slideshow_index_to_current(void)
{
    size_t index = 0;
    if (s_state != NULL &&
        album_index_from_media_id(s_state->current_media_id, &index)) {
        s_slideshow_index = index;
        return;
    }
    if (first_displayable_photo_index(&index)) {
        s_slideshow_index = index;
    } else {
        s_slideshow_index = 0;
    }
}

static void align_slideshow_index_after_current(void)
{
    size_t index = 0;
    if (s_state != NULL &&
        s_album != NULL &&
        s_album->count > 0 &&
        album_index_from_media_id(s_state->current_media_id, &index)) {
        s_slideshow_index = (index + 1) % s_album->count;
        return;
    }
    align_slideshow_index_to_current();
}

static void show_selected_photo(const char *status)
{
    char path[PHOTO_STORAGE_MAX_PATH];

    if (s_state == NULL || s_album == NULL) {
        return;
    }

    if (current_photo_path(path, sizeof(path))) {
        ESP_LOGI(PHOTO_MQTT_TAG, "show current photo path=%s current_media_id=%d", path, s_state->current_media_id);
        show_local_photo(path, status);
        return;
    }

    if (s_state->current_media_id > 0 && can_use_cloud_photos()) {
        snprintf(s_state->status, sizeof(s_state->status), "正在同步选中的照片");
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(s_state->device_uid, s_state->status, s_state->media_count);
            photo_ui_set_brightness(s_state->brightness);
            lvgl_port_unlock();
        }
        return;
    }

    if (s_album->count > 0) {
        size_t index = 0;
        if (first_displayable_photo_index(&index)) {
            ESP_LOGI(PHOTO_MQTT_TAG, "show first local photo path=%s index=%u", s_album->paths[index], (unsigned)index);
            update_current_media_from_path(s_album->paths[index]);
            show_local_photo(s_album->paths[index], status);
        }
        return;
    }

    if (lvgl_port_lock(100)) {
        photo_ui_show_cloud(s_state->device_uid, status != NULL ? status : s_state->status, s_state->media_count);
        photo_ui_set_brightness(s_state->brightness);
        lvgl_port_unlock();
    }
}

void photo_mqtt_show_selected(const char *status)
{
    show_selected_photo(status);
}

static void apply_brightness(void)
{
    if (s_state == NULL) {
        return;
    }
    if (lvgl_port_lock(100)) {
        photo_ui_set_brightness(s_state->brightness);
        lvgl_port_unlock();
    }
}

static void stop_slideshow_task(void)
{
    if (s_slideshow_task == NULL) {
        return;
    }
    s_slideshow_stop_requested = true;
    if (xTaskGetCurrentTaskHandle() == s_slideshow_task) {
        return;
    }
    for (int waited = 0; waited < 20 && s_slideshow_task != NULL; waited++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_slideshow_task != NULL) {
        ESP_LOGW(PHOTO_MQTT_TAG, "slideshow task did not stop in time");
    }
}

void photo_mqtt_pause_slideshow(void)
{
    stop_slideshow_task();
}

void photo_mqtt_apply_unbound(void)
{
    if (s_state == NULL) {
        return;
    }
    stop_slideshow_task();
    s_state->frame_id[0] = '\0';
    photo_cloud_set_bound(s_state, false);
    s_state->active_album_id = 0;
    s_state->current_media_id = 0;
    s_state->media_count = 0;
    s_state->downloaded_count = 0;
    snprintf(s_state->play_mode, sizeof(s_state->play_mode), "single");
    snprintf(s_state->status, sizeof(s_state->status), "相框已解绑，请在 App 里重新绑定");
    persist_cloud_state();
    if (s_album != NULL) {
        photo_storage_clear_cloud_cache(s_album);
    }
    if (lvgl_port_lock(100)) {
        photo_ui_show_cloud(s_state->device_uid, s_state->status, 0);
        photo_ui_set_brightness(s_state->brightness);
        lvgl_port_unlock();
    }
    publish_state("unbound");
    publish_event("unbound_done", NULL);
    ESP_LOGI(PHOTO_MQTT_TAG, "frame unbound; restart into BLE provisioning");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void slideshow_task(void *arg)
{
    (void)arg;
    char path[PHOTO_STORAGE_MAX_PATH];
    while (!s_slideshow_stop_requested &&
           s_state != NULL && s_album != NULL && strcasecmp(s_state->play_mode, "slideshow") == 0) {
        bool prefer_cloud = can_use_cloud_photos() && has_cloud_photos();
        bool shown = false;
        /* 每次都在持锁下读取 count/index/path 并复制到本地，
         * 避免与 photo_storage_scan 并发时除零或读到被重写的路径。 */
        for (size_t scanned = 0;; scanned++) {
            if (s_slideshow_stop_requested) {
                goto done;
            }
            photo_storage_lock();
            size_t count = s_album->count;
            if (count == 0) {
                photo_storage_unlock();
                goto done;
            }
            if (scanned >= count) {
                photo_storage_unlock();
                break;
            }
            size_t index = s_slideshow_index % count;
            snprintf(path, sizeof(path), "%s", s_album->paths[index]);
            s_slideshow_index = (index + 1) % count;
            photo_storage_unlock();

            bool is_cloud = is_cloud_photo_path(path);
            if ((prefer_cloud && !is_cloud) || (!prefer_cloud && is_cloud)) {
                continue;
            }

            int media_id = 0;
            media_id_from_cloud_path(path, &media_id);
            ESP_LOGI(PHOTO_MQTT_TAG, "slideshow tick index=%u path=%s media_id=%d prefer_cloud=%d current_media_id=%d",
                     (unsigned)index, path, media_id, prefer_cloud ? 1 : 0,
                     s_state != NULL ? s_state->current_media_id : 0);
            update_current_media_from_path(path);
            show_local_photo(path, "轮播中");
            publish_state("slideshow_tick");
            shown = true;
            break;
        }

        if (!shown) {
            break;
        }

        int delay_sec = s_state->slideshow_interval_sec > 0 ? s_state->slideshow_interval_sec : 15;
        for (int elapsed = 0; elapsed < delay_sec * 10; elapsed++) {
            if (s_slideshow_stop_requested ||
                s_state == NULL || strcasecmp(s_state->play_mode, "slideshow") != 0) {
                goto done;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

done:
    s_slideshow_task = NULL;
    s_slideshow_stop_requested = false;
    vTaskDelete(NULL);
}

static void start_slideshow_task(bool align_to_current)
{
    stop_slideshow_task();
    if (s_slideshow_task != NULL) {
        return;
    }
    if (s_state == NULL || s_album == NULL || s_album->count == 0 || strcasecmp(s_state->play_mode, "slideshow") != 0) {
        return;
    }
    s_slideshow_stop_requested = false;
    if (align_to_current) {
        align_slideshow_index_to_current();
    }
    if (s_slideshow_index >= s_album->count) {
        s_slideshow_index = 0;
    }
    log_slideshow_state(align_to_current ? "start_slideshow_aligned" : "start_slideshow");
    if (xTaskCreate(slideshow_task, "photo_slideshow", 4096, NULL, 5, &s_slideshow_task) != pdPASS) {
        s_slideshow_task = NULL;
        ESP_LOGW(PHOTO_MQTT_TAG, "slideshow task create failed");
    }
}

void photo_mqtt_apply_playback(const char *status)
{
    if (s_state != NULL && s_state->binding_state_known && !s_state->bound &&
        has_cloud_photos() && !has_manual_photos()) {
        stop_slideshow_task();
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(s_state->device_uid, s_state->status, 0);
            photo_ui_set_brightness(s_state->brightness);
            lvgl_port_unlock();
        }
        return;
    }
    if (s_state != NULL && strcasecmp(s_state->play_mode, "slideshow") == 0) {
        start_slideshow_task(true);
    } else {
        show_selected_photo(status);
    }
}

static void sync_catalog_now(void)
{
    if (s_state == NULL || s_album == NULL) {
        return;
    }

    s_sync_running = true;
    stop_slideshow_task();
    esp_err_t ret = photo_cloud_sync_once(s_state, s_album);
    if (ret == ESP_OK) {
        publish_event("sync_catalog_done", NULL);
        if (!s_state->bound) {
            photo_mqtt_apply_playback(NULL);
        } else if (strcasecmp(s_state->play_mode, "slideshow") == 0) {
            log_slideshow_state("sync_catalog_before_slideshow_restart");
            start_slideshow_task(true);
        } else {
            show_selected_photo(s_state->status);
        }
    } else {
        publish_event("sync_catalog_failed", cJSON_CreateString(s_state->status));
        if (s_album != NULL && s_album->count > 0) {
            photo_mqtt_apply_playback(s_state->status);
        } else {
            if (lvgl_port_lock(100)) {
                photo_ui_show_cloud(s_state->device_uid, s_state->status, s_state->media_count);
                photo_ui_set_brightness(s_state->brightness);
                lvgl_port_unlock();
            }
        }
    }
    publish_state("sync_catalog");
    s_sync_running = false;
}

static void sync_catalog_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        do {
            s_sync_requested = false;
            sync_catalog_now();
        } while (s_sync_requested);
        UBaseType_t remaining = uxTaskGetStackHighWaterMark(NULL);
        if (remaining < 1024) {
            ESP_LOGW(PHOTO_MQTT_TAG, "sync catalog stack is low: %u bytes remaining",
                     (unsigned)remaining);
        }
    }
}

static void request_sync_catalog(void)
{
    if (s_sync_running) {
        s_sync_requested = true;
        ESP_LOGI(PHOTO_MQTT_TAG, "cloud sync is running; merge request");
        return;
    }
    s_sync_requested = true;
    if (!ensure_sync_catalog_task()) {
        s_sync_requested = false;
        return;
    }
    xTaskNotifyGive(s_sync_task);
}

static bool ensure_sync_catalog_task(void)
{
    if (s_sync_task == NULL) {
        s_sync_task = xTaskCreateStatic(sync_catalog_task, "photo_sync_catalog",
                                        PHOTO_MQTT_SYNC_TASK_STACK, NULL, 5,
                                        s_sync_task_stack, &s_sync_task_buffer);
        if (s_sync_task == NULL) {
            ESP_LOGW(PHOTO_MQTT_TAG, "sync catalog task create failed");
            return false;
        }
    }
    return true;
}

static void apply_update_settings(cJSON *data)
{
    if (s_state == NULL || data == NULL) {
        return;
    }
    bool was_slideshow = strcasecmp(s_state->play_mode, "slideshow") == 0;
    int previous_current_media_id = s_state->current_media_id;
    int previous_active_album_id = s_state->active_album_id;

    cJSON *brightness = cJSON_GetObjectItem(data, "brightness");
    cJSON *interval = cJSON_GetObjectItem(data, "slideshow_interval_sec");
    cJSON *play_mode = cJSON_GetObjectItem(data, "play_mode");
    cJSON *current_media_id = cJSON_GetObjectItem(data, "current_media_id");
    cJSON *active_album_id = cJSON_GetObjectItem(data, "active_album_id");

    if (cJSON_IsNumber(brightness)) {
        s_state->brightness = brightness->valueint;
    }
    if (cJSON_IsNumber(interval) && interval->valueint > 0) {
        s_state->slideshow_interval_sec = interval->valueint;
    }
    if (cJSON_IsString(play_mode) &&
        (strcmp(play_mode->valuestring, "single") == 0 || strcmp(play_mode->valuestring, "slideshow") == 0)) {
        snprintf(s_state->play_mode, sizeof(s_state->play_mode), "%s", play_mode->valuestring);
    }
    if (cJSON_IsNumber(current_media_id)) {
        s_state->current_media_id = current_media_id->valueint;
    }
    if (cJSON_IsNumber(active_album_id)) {
        if (s_state->active_album_id != active_album_id->valueint) {
            s_slideshow_index = 0;
        }
        s_state->active_album_id = active_album_id->valueint;
    }

    if (previous_active_album_id != s_state->active_album_id && s_album != NULL) {
        stop_slideshow_task();
        photo_storage_set_album_scope(s_album, s_state->active_album_id);
        photo_storage_scan(s_album);
    }
    persist_cloud_state();

    apply_brightness();
    log_slideshow_state("apply_update_settings");
    if (strcasecmp(s_state->play_mode, "slideshow") == 0) {
        bool should_align =
            !was_slideshow ||
            previous_current_media_id != s_state->current_media_id ||
            previous_active_album_id != s_state->active_album_id;
        if (should_align || s_slideshow_task == NULL) {
            start_slideshow_task(should_align);
        }
        if (previous_active_album_id != s_state->active_album_id) {
            request_sync_catalog();
        }
    } else {
        stop_slideshow_task();
        char path[PHOTO_STORAGE_MAX_PATH];
        if (s_state->current_media_id > 0 && !current_photo_path(path, sizeof(path))) {
            request_sync_catalog();
        } else {
            show_selected_photo("固定播放");
        }
    }
    publish_state("update_settings");
    publish_event("update_settings_done", NULL);
}

static void apply_set_current_media(cJSON *data)
{
    if (s_state == NULL || data == NULL) {
        return;
    }
    cJSON *media_id = cJSON_GetObjectItem(data, "media_id");
    cJSON *active_album_id = cJSON_GetObjectItem(data, "active_album_id");
    if (!cJSON_IsNumber(media_id)) {
        return;
    }
    s_state->current_media_id = media_id->valueint;
    if (cJSON_IsNumber(active_album_id)) {
        s_state->active_album_id = active_album_id->valueint;
        if (s_album != NULL) {
            stop_slideshow_task();
            photo_storage_set_album_scope(s_album, s_state->active_album_id);
            photo_storage_scan(s_album);
        }
    }
    snprintf(s_state->play_mode, sizeof(s_state->play_mode), "single");
    persist_cloud_state();
    stop_slideshow_task();
    log_slideshow_state("apply_set_current_media");
    char path[PHOTO_STORAGE_MAX_PATH];
    if (current_photo_path(path, sizeof(path))) {
        show_selected_photo("当前照片");
    } else {
        snprintf(s_state->status, sizeof(s_state->status), "正在同步选中的照片");
        publish_event("selected_photo_cache_miss", cJSON_CreateNumber(s_state->current_media_id));
        request_sync_catalog();
    }
    publish_state("set_current_media");
    publish_event("set_current_media_done", NULL);
}

static void remove_media_file(int media_id)
{
    if (s_album != NULL) {
        photo_storage_lock();
        for (size_t i = 0; i < s_album->count; i++) {
            int path_media_id = 0;
            if (media_id_from_cloud_path(s_album->paths[i], &path_media_id) && path_media_id == media_id) {
                ESP_LOGI(PHOTO_MQTT_TAG, "remove cached media id=%d path=%s", media_id, s_album->paths[i]);
                remove(s_album->paths[i]);
                photo_storage_unlock();
                return;
            }
        }
        photo_storage_unlock();
    }
}

static void apply_delete_media(cJSON *data)
{
    if (s_state == NULL || s_album == NULL || data == NULL) {
        return;
    }
    cJSON *media_id = cJSON_GetObjectItem(data, "media_id");
    if (!cJSON_IsNumber(media_id)) {
        return;
    }
    stop_slideshow_task();
    remove_media_file(media_id->valueint);
    photo_storage_scan(s_album);
    if (s_state->current_media_id == media_id->valueint) {
        s_state->current_media_id = 0;
        s_slideshow_index = 0;
    }
    persist_cloud_state();
    log_slideshow_state("apply_delete_media");
    if (strcasecmp(s_state->play_mode, "slideshow") == 0 && s_album->count > 0) {
        start_slideshow_task(true);
    } else {
        show_selected_photo("照片已删除");
    }
    publish_state("delete_media");
    publish_event("delete_media_done", NULL);
}

static void apply_delete_album(cJSON *data)
{
    if (s_state == NULL || s_album == NULL || data == NULL) {
        return;
    }
    stop_slideshow_task();
    cJSON *media_ids = cJSON_GetObjectItem(data, "media_ids");
    if (cJSON_IsArray(media_ids)) {
        int count = cJSON_GetArraySize(media_ids);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(media_ids, i);
            if (cJSON_IsNumber(item)) {
                remove_media_file(item->valueint);
            }
        }
    }
    photo_storage_scan(s_album);
    if (s_state->current_media_id > 0) {
        size_t index = 0;
        if (!album_index_from_media_id(s_state->current_media_id, &index)) {
            s_state->current_media_id = 0;
            s_slideshow_index = 0;
        }
    }
    persist_cloud_state();
    log_slideshow_state("apply_delete_album");
    if (strcasecmp(s_state->play_mode, "slideshow") == 0 && s_album->count > 0) {
        start_slideshow_task(true);
    } else {
        show_selected_photo("相册已删除");
    }
    publish_state("delete_album");
    publish_event("delete_album_done", NULL);
}

static void handle_command_payload(const char *payload, size_t payload_len)
{
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (root == NULL) {
        return;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    if (!remember_command_id(root) || command_is_stale(root, data)) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "sync_catalog") == 0) {
        request_sync_catalog();
    } else if (strcmp(type->valuestring, "set_current_media") == 0) {
        remember_command_versions(root, data);
        apply_set_current_media(data);
        persist_cloud_state();
    } else if (strcmp(type->valuestring, "update_settings") == 0) {
        remember_command_versions(root, data);
        apply_update_settings(data);
        persist_cloud_state();
    } else if (strcmp(type->valuestring, "delete_media") == 0) {
        remember_command_versions(root, data);
        apply_delete_media(data);
        persist_cloud_state();
    } else if (strcmp(type->valuestring, "delete_album") == 0) {
        remember_command_versions(root, data);
        apply_delete_album(data);
        persist_cloud_state();
    } else if (strcmp(type->valuestring, "unbound") == 0) {
        remember_command_versions(root, data);
        photo_mqtt_apply_unbound();
        persist_cloud_state();
    }
    cJSON_Delete(root);
}

static void command_worker_task(void *arg)
{
    (void)arg;
    photo_mqtt_command_t command;
    while (true) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        handle_command_payload(command.payload, command.len);
        free(command.payload);

        UBaseType_t remaining = uxTaskGetStackHighWaterMark(NULL);
        if (remaining < 512) {
            ESP_LOGW(PHOTO_MQTT_TAG, "command worker stack is low: %u words remaining",
                     (unsigned)remaining);
        }
    }
}

static void ensure_command_worker(void)
{
    if (s_command_queue == NULL) {
        s_command_queue = xQueueCreate(PHOTO_MQTT_CMD_QUEUE_LEN, sizeof(photo_mqtt_command_t));
        if (s_command_queue == NULL) {
            ESP_LOGW(PHOTO_MQTT_TAG, "command queue create failed");
            return;
        }
    }
    if (s_command_task == NULL) {
        if (xTaskCreate(command_worker_task, "photo_mqtt_cmd", PHOTO_MQTT_CMD_TASK_STACK,
                        NULL, 5, &s_command_task) != pdPASS) {
            ESP_LOGW(PHOTO_MQTT_TAG, "command worker create failed");
            s_command_task = NULL;
        }
    }
}

static void enqueue_command_payload(const char *payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0) {
        return;
    }
    if (payload_len >= PHOTO_MQTT_CMD_PAYLOAD_MAX) {
        ESP_LOGW(PHOTO_MQTT_TAG, "drop oversized command payload len=%u", (unsigned)payload_len);
        return;
    }
    ensure_command_worker();
    if (s_command_queue == NULL || s_command_task == NULL) {
        return;
    }

    photo_mqtt_command_t command = {0};
    command.len = payload_len;
    command.payload = malloc(payload_len + 1);
    if (command.payload == NULL) {
        ESP_LOGW(PHOTO_MQTT_TAG, "drop command because payload alloc failed");
        return;
    }
    memcpy(command.payload, payload, payload_len);
    command.payload[payload_len] = '\0';
    if (xQueueSend(s_command_queue, &command, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(PHOTO_MQTT_TAG, "drop command because queue is full");
        free(command.payload);
    }
}

static void mqtt_event_handler_cb(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(PHOTO_MQTT_TAG, "MQTT connected");
        s_mqtt_connected = true;
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        publish_state("mqtt_connected");
        if (!s_mqtt_first_connect_handled) {
            s_mqtt_first_connect_handled = true;
            if (!s_state->cloud_sync_ready) {
                request_sync_catalog();
            } else {
                ESP_LOGI(PHOTO_MQTT_TAG, "skip first MQTT connect sync because startup sync is ready");
            }
        } else {
            request_sync_catalog();
        }
        break;
    case MQTT_EVENT_DATA: {
        char topic[96];
        int topic_len = event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';
        if (strcmp(topic, s_cmd_topic) == 0) {
            enqueue_command_payload(event->data, event->data_len);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(PHOTO_MQTT_TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;
    default:
        break;
    }
}

static void build_topics(photo_cloud_state_t *state)
{
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "photo/frame/%s/cmd", state->device_uid);
    snprintf(s_state_topic, sizeof(s_state_topic), "photo/frame/%s/state", state->device_uid);
    snprintf(s_event_topic, sizeof(s_event_topic), "photo/frame/%s/event", state->device_uid);
}

void photo_mqtt_attach(photo_cloud_state_t *state, photo_album_t *album)
{
    s_state = state;
    s_album = album;
    if (state != NULL && state->device_uid[0] != '\0') {
        build_topics(state);
    }
}

void photo_mqtt_start(photo_cloud_state_t *state, photo_album_t *album)
{
    if (state == NULL || state->device_uid[0] == '\0' || CONFIG_PHOTO_MQTT_BROKER_URL[0] == '\0') {
        return;
    }
    photo_mqtt_attach(state, album);
    ensure_command_worker();
    ensure_state_heartbeat_task();
    ensure_sync_catalog_task();
    s_mqtt_first_connect_handled = false;
    s_mqtt_connected = false;

    esp_mqtt_client_config_t config = {
        .broker.address.uri = CONFIG_PHOTO_MQTT_BROKER_URL,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = state->device_uid,
        .session.keepalive = 30,
    };
    s_client = esp_mqtt_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGW(PHOTO_MQTT_TAG, "MQTT init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler_cb, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(PHOTO_MQTT_TAG, "MQTT start broker=%s cmd_topic=%s", CONFIG_PHOTO_MQTT_BROKER_URL, s_cmd_topic);
}
