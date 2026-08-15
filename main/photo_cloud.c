#include "photo_cloud.h"

#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "photo_ui.h"
#include "protocomm_ble.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_PROV_END_BIT BIT2
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_PROVISION_TIMEOUT_MS (10 * 60 * 1000)
#define WIFI_PROVISION_SUCCESS_GRACE_MS 3000
#define WIFI_PROVISION_STOP_WAIT_MS 5000
#define FRAME_BINDING_WAIT_AFTER_WIFI_MS (2 * 60 * 1000)
#define FRAME_BINDING_POLL_MS 3000
#define WIFI_MAX_RETRY 5
#define HTTP_RESPONSE_MAX 32768
#define HTTP_ERROR_PREVIEW_MAX 256
#define HTTP_EMPTY_READ_MAX 10
#define HTTP_EMPTY_READ_DELAY_MS 100
#define DEVICE_MEDIA_PAGE_LIMIT 20
#define MEDIA_URL_MAX 2048
#define MEDIA_PATH_MAX PHOTO_STORAGE_MAX_PATH
#define PHOTO_SD_MIN_FREE_AFTER_DOWNLOAD (256 * 1024)
#define PHOTO_NVS_NAMESPACE "photo_frame"
#define PHOTO_NVS_BOUND_KEY "bound"
#define PHOTO_NVS_ACTIVE_ALBUM_KEY "album_id"
#define PHOTO_NVS_CURRENT_MEDIA_KEY "media_id"
#define PHOTO_NVS_BRIGHTNESS_KEY "brightness"
#define PHOTO_NVS_INTERVAL_KEY "interval"
#define PHOTO_NVS_PLAY_MODE_KEY "play_mode"
#define PHOTO_NVS_CONFIG_VERSION_KEY "config_ver"
#define PHOTO_NVS_CONTENT_VERSION_KEY "content_ver"

static const char *TAG = "photo_cloud";

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;
static bool s_nvs_ready;
static bool s_netif_ready;
static photo_cloud_state_t *s_active_state;
static SemaphoreHandle_t s_cloud_sync_mutex;

typedef struct {
    int ids[PHOTO_STORAGE_MAX_FILES];
    int count;
    int total;
} cloud_media_manifest_t;

static bool cloud_media_id_from_name(const char *name, int *media_id);
static void show_provisioning_status(photo_cloud_state_t *state);
static cJSON *response_data(const char *json);
static bool parse_config(photo_cloud_state_t *state, cJSON *data);
static bool wait_for_frame_binding(photo_cloud_state_t *state, const char *url, char *response, size_t response_size);

static void set_status(photo_cloud_state_t *state, const char *status)
{
    snprintf(state->status, sizeof(state->status), "%s", status);
}

static void log_crypto_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "%s heap internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
             stage,
             (unsigned)internal_free,
             (unsigned)internal_largest,
             (unsigned)spiram_free,
             (unsigned)spiram_largest);
}

static esp_err_t warmup_ble_security1_rng(void)
{
    uint8_t rng_probe[32] = {0};
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    int mbed_err;

    esp_fill_random(rng_probe, sizeof(rng_probe));
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    mbed_err = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (mbed_err == 0) {
        mbed_err = mbedtls_ctr_drbg_random(&ctr_drbg, rng_probe, sizeof(rng_probe));
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (mbed_err != 0) {
        ESP_LOGE(TAG, "BLE Security 1 RNG warmup failed: -0x%x", -mbed_err);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t prepare_ble_security1(photo_cloud_state_t *state)
{
    esp_err_t ret = ESP_OK;

    log_crypto_heap("before BLE Security 1 warmup");
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = warmup_ble_security1_rng();
        if (ret == ESP_OK) {
            log_crypto_heap("after BLE Security 1 warmup");
            return ESP_OK;
        }
        log_crypto_heap("BLE Security 1 warmup failed");
        ESP_LOGW(TAG, "BLE Security 1 warmup attempt %d failed: %s", attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    set_status(state, "蓝牙加密初始化失败，请重启相框后重试");
    show_provisioning_status(state);
    return ret;
}

static void show_provisioning_status(photo_cloud_state_t *state)
{
    if (state == NULL || !state->provisioning_active) {
        return;
    }

    if (lvgl_port_lock(100)) {
#if CONFIG_PHOTO_BLE_SECURITY_1
        photo_ui_show_provisioning(state->device_uid, state->provisioning_name, CONFIG_PHOTO_BLE_POP, state->status, state->binding_state_known, state->bound);
#else
        photo_ui_show_provisioning(state->device_uid, state->provisioning_name, NULL, state->status, state->binding_state_known, state->bound);
#endif
        lvgl_port_unlock();
    }
}

static esp_err_t ensure_nvs_ready(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init failed");
    s_nvs_ready = true;
    return ESP_OK;
}

void photo_cloud_load_binding_state(photo_cloud_state_t *state)
{
    nvs_handle_t handle;
    uint8_t bound = 0;
    int32_t value = 0;
    if (state == NULL) {
        return;
    }
    if (ensure_nvs_ready() != ESP_OK) {
        state->bound = false;
        state->binding_state_known = false;
        return;
    }
    if (nvs_open(PHOTO_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        state->bound = false;
        state->binding_state_known = false;
        return;
    }
    if (nvs_get_u8(handle, PHOTO_NVS_BOUND_KEY, &bound) == ESP_OK) {
        state->bound = bound != 0;
        state->binding_state_known = true;
    } else {
        state->bound = false;
        state->binding_state_known = false;
    }
    if (nvs_get_i32(handle, PHOTO_NVS_CONFIG_VERSION_KEY, &value) == ESP_OK && value > 0) {
        state->config_version = value;
    }
    if (nvs_get_i32(handle, PHOTO_NVS_CONTENT_VERSION_KEY, &value) == ESP_OK && value > 0) {
        state->content_version = value;
    }
    if (state->binding_state_known && state->bound) {
        if (nvs_get_i32(handle, PHOTO_NVS_ACTIVE_ALBUM_KEY, &value) == ESP_OK) {
            state->active_album_id = value;
        }
        if (nvs_get_i32(handle, PHOTO_NVS_CURRENT_MEDIA_KEY, &value) == ESP_OK) {
            state->current_media_id = value;
        }
        if (nvs_get_i32(handle, PHOTO_NVS_BRIGHTNESS_KEY, &value) == ESP_OK) {
            state->brightness = value;
        }
        if (nvs_get_i32(handle, PHOTO_NVS_INTERVAL_KEY, &value) == ESP_OK) {
            state->slideshow_interval_sec = value;
        }
        size_t play_mode_len = sizeof(state->play_mode);
        (void)nvs_get_str(handle, PHOTO_NVS_PLAY_MODE_KEY, state->play_mode, &play_mode_len);
    }
    nvs_close(handle);
}

void photo_cloud_save_cached_state(photo_cloud_state_t *state)
{
    nvs_handle_t handle;
    if (state == NULL) {
        return;
    }
    if (ensure_nvs_ready() != ESP_OK) {
        ESP_LOGW(TAG, "nvs is not ready while saving cloud state");
        return;
    }
    if (nvs_open(PHOTO_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed while saving cloud state");
        return;
    }
    (void)nvs_set_i32(handle, PHOTO_NVS_ACTIVE_ALBUM_KEY, state->active_album_id);
    (void)nvs_set_i32(handle, PHOTO_NVS_CURRENT_MEDIA_KEY, state->current_media_id);
    (void)nvs_set_i32(handle, PHOTO_NVS_BRIGHTNESS_KEY, state->brightness);
    (void)nvs_set_i32(handle, PHOTO_NVS_INTERVAL_KEY, state->slideshow_interval_sec);
    (void)nvs_set_str(handle, PHOTO_NVS_PLAY_MODE_KEY, state->play_mode);
    (void)nvs_set_i32(handle, PHOTO_NVS_CONFIG_VERSION_KEY, state->config_version);
    (void)nvs_set_i32(handle, PHOTO_NVS_CONTENT_VERSION_KEY, state->content_version);
    esp_err_t ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save cloud state failed: %s", esp_err_to_name(ret));
    }
    nvs_close(handle);
}

static void save_binding_state(photo_cloud_state_t *state, bool bound)
{
    if (state->binding_state_known && state->bound == bound) {
        return;
    }
    state->bound = bound;
    state->binding_state_known = true;

    nvs_handle_t handle;
    if (ensure_nvs_ready() != ESP_OK) {
        ESP_LOGW(TAG, "nvs is not ready while saving binding state");
        return;
    }
    if (nvs_open(PHOTO_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed while saving binding state");
        return;
    }
    esp_err_t ret = nvs_set_u8(handle, PHOTO_NVS_BOUND_KEY, bound ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save binding state failed: %s", esp_err_to_name(ret));
    }
    nvs_close(handle);
}

void photo_cloud_set_bound(photo_cloud_state_t *state, bool bound)
{
    if (state == NULL) {
        return;
    }
    save_binding_state(state, bound);
}

static const char *device_secret_header_value(void)
{
    if (strlen(CONFIG_PHOTO_DEVICE_SECRET) > 0) {
        return CONFIG_PHOTO_DEVICE_SECRET;
    }
    return NULL;
}

static void make_provisioning_name(photo_cloud_state_t *state, const uint8_t mac[6])
{
    snprintf(state->provisioning_name, sizeof(state->provisioning_name),
             "PF-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void format_mac_uid(char *out, size_t out_size, const uint8_t mac[6])
{
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void photo_cloud_init_state(photo_cloud_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->brightness = 70;
    state->slideshow_interval_sec = 15;
    snprintf(state->play_mode, sizeof(state->play_mode), "single");
    state->api_configured = strlen(CONFIG_PHOTO_API_BASE_URL) > 0;

    uint8_t sta_mac[6] = {0};
    uint8_t bt_mac[6] = {0};
    bool has_sta_mac = esp_read_mac(sta_mac, ESP_MAC_WIFI_STA) == ESP_OK;
    bool has_bt_mac = esp_read_mac(bt_mac, ESP_MAC_BT) == ESP_OK;

    if (strlen(CONFIG_PHOTO_FRAME_DEVICE_UID) > 0) {
        snprintf(state->device_uid, sizeof(state->device_uid), "%s", CONFIG_PHOTO_FRAME_DEVICE_UID);
    } else if (has_bt_mac) {
        format_mac_uid(state->device_uid, sizeof(state->device_uid), bt_mac);
    } else if (has_sta_mac) {
        format_mac_uid(state->device_uid, sizeof(state->device_uid), sta_mac);
    } else {
        snprintf(state->device_uid, sizeof(state->device_uid), "PF-UNSET");
    }

    if (has_sta_mac) {
        make_provisioning_name(state, sta_mac);
    } else if (has_bt_mac) {
        make_provisioning_name(state, bt_mac);
    } else {
        snprintf(state->provisioning_name, sizeof(state->provisioning_name), "PF-SETUP");
    }

    set_status(state, "云端同步未开始");
}

static void system_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    photo_cloud_state_t *state = s_active_state;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            if (state != NULL) {
                set_status(state, "蓝牙配网已开启");
                show_provisioning_status(state);
            }
            break;
        case WIFI_PROV_CRED_RECV:
            if (state != NULL) {
                set_status(state, "已收到 Wi-Fi 信息");
                show_provisioning_status(state);
            }
            break;
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            if (state != NULL) {
                set_status(state, (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                           "Wi-Fi 密码错误，请在 App 里重试" :
                           "找不到这个 Wi-Fi，请在 App 里重试");
                show_provisioning_status(state);
            }
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            if (state != NULL) {
                set_status(state, "配网成功，正在联网");
                show_provisioning_status(state);
            }
            break;
        case WIFI_PROV_END:
            if (state != NULL && state->provisioning_active) {
                set_status(state, "配网完成，正在同步云端");
                show_provisioning_status(state);
                state->provisioning_active = false;
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_PROV_END_BIT);
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (state != NULL && state->provisioning_active) {
            if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
                log_crypto_heap("BLE connected");
                set_status(state, "手机已连接蓝牙");
                show_provisioning_status(state);
            } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
                log_crypto_heap("BLE disconnected");
                set_status(state, "蓝牙配网中，等待 App 连接");
                show_provisioning_status(state);
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAX_RETRY) {
            s_wifi_retry_count++;
            esp_wifi_connect();
            return;
        }

        if (state != NULL && state->provisioning_active) {
            s_wifi_retry_count = 0;
            set_status(state, "Wi-Fi 连接失败，请在 App 里重试");
            show_provisioning_status(state);
            return;
        }

        if (state != NULL) {
            state->wifi_connected = false;
            set_status(state, "Wi-Fi 连接失败");
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        if (state != NULL) {
            state->wifi_connected = true;
            state->wifi_provisioned = true;
            if (state->provisioning_active) {
                set_status(state, "Wi-Fi 已连接，正在完成配网");
                show_provisioning_status(state);
            } else {
                set_status(state, "Wi-Fi 已连接");
            }
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_netif_ready(void)
{
    if (s_netif_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_netif_create_default_wifi_sta();
    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &system_event_handler, NULL),
                        TAG, "register provisioning handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                                   &system_event_handler, NULL),
                        TAG, "register ble handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &system_event_handler, NULL),
                        TAG, "register wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &system_event_handler, NULL),
                        TAG, "register ip handler failed");
    s_netif_ready = true;
    return ESP_OK;
}

static bool wifi_has_saved_credentials(void)
{
    wifi_config_t wifi_cfg = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
           strlen((const char *)wifi_cfg.sta.ssid) > 0;
}

static esp_err_t wait_for_wifi_connected(photo_cloud_state_t *state, TickType_t ticks_to_wait)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, ticks_to_wait);
    if (bits & WIFI_CONNECTED_BIT) {
        state->wifi_connected = true;
        set_status(state, "Wi-Fi 已连接");
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        set_status(state, "Wi-Fi 连接失败");
        return ESP_ERR_TIMEOUT;
    }

    set_status(state, "Wi-Fi 连接超时");
    return ESP_ERR_TIMEOUT;
}

static bool wifi_station_has_link(void)
{
    wifi_ap_record_t ap_info = {0};
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

static void wait_for_provisioning_stopped(photo_cloud_state_t *state, bool request_stop)
{
    if (state == NULL || !state->provisioning_active) {
        return;
    }

    if (!request_stop) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_PROV_END_BIT,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_PROVISION_SUCCESS_GRACE_MS));
        if ((bits & WIFI_PROV_END_BIT) != 0 || !state->provisioning_active) {
            state->provisioning_active = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            return;
        }
        request_stop = true;
    }

    if (request_stop) {
        wifi_prov_mgr_stop_provisioning();
    }
    ESP_LOGI(TAG, "wait for provisioning stop before cloud sync");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_PROV_END_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_PROVISION_STOP_WAIT_MS));
    if ((bits & WIFI_PROV_END_BIT) == 0) {
        ESP_LOGW(TAG, "provisioning stop event not observed; continue after guard delay");
        state->provisioning_active = false;
    } else {
        ESP_LOGI(TAG, "provisioning stopped; continue cloud sync");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
}

static esp_err_t start_ble_provisioning(photo_cloud_state_t *state)
{
#if CONFIG_PHOTO_BLE_SECURITY_1
    if (strlen(CONFIG_PHOTO_BLE_POP) == 0) {
        state->setup_blocked = true;
        state->provisioning_active = false;
        state->wifi_provisioned = false;
        set_status(state, "设备未写入配网码，请联系售后");
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(state->device_uid, state->status, -1);
            lvgl_port_unlock();
        }
        ESP_LOGE(TAG, "BLE Security 1 requires a per-frame POP");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };

    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(config), TAG, "provisioning manager init failed");

    state->provisioning_timed_out = false;
    state->provisioning_active = true;
    set_status(state, "蓝牙配网准备中");
    show_provisioning_status(state);

    esp_err_t ret = ESP_OK;
#if CONFIG_PHOTO_BLE_SECURITY_1
    ret = prepare_ble_security1(state);
    if (ret != ESP_OK) {
        wifi_prov_mgr_deinit();
        state->provisioning_active = false;
        return ret;
    }
#endif

    set_status(state, "蓝牙配网已开启");
    show_provisioning_status(state);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_PROV_END_BIT);
#if CONFIG_PHOTO_BLE_SECURITY_1
    const wifi_prov_security_t security = WIFI_PROV_SECURITY_1;
    const void *security_params = CONFIG_PHOTO_BLE_POP;
#else
    const wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
    const void *security_params = NULL;
#endif
    ret = wifi_prov_mgr_start_provisioning(security, security_params,
                                           state->provisioning_name, NULL);
    if (ret != ESP_OK) {
        wifi_prov_mgr_deinit();
        state->provisioning_active = false;
        return ret;
    }
    log_crypto_heap("after BLE provisioning started");

    ret = wait_for_wifi_connected(state, pdMS_TO_TICKS(WIFI_PROVISION_TIMEOUT_MS));
    if (ret != ESP_OK) {
        set_status(state, "蓝牙配网已超时，请在 App 里重新进入配网");
        show_provisioning_status(state);
        ESP_LOGW(TAG, "BLE provisioning timeout; stop provisioning");
        wait_for_provisioning_stopped(state, true);
        state->provisioning_active = false;
        state->provisioning_timed_out = true;
    } else {
        state->wifi_just_provisioned = true;
        wait_for_provisioning_stopped(state, false);
    }
    return ret;
}

static esp_err_t connect_saved_wifi(photo_cloud_state_t *state)
{
    state->wifi_provisioned = true;
    state->provisioning_active = false;

    if (wifi_station_has_link()) {
        state->wifi_connected = true;
        set_status(state, "Wi-Fi 已连接");
        return ESP_OK;
    }

    set_status(state, "正在连接上次的 Wi-Fi");
    if (lvgl_port_lock(100)) {
        photo_ui_show_wifi_reconnect(WIFI_CONNECT_TIMEOUT_MS / 1000);
        lvgl_port_unlock();
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi start returned %s; try connect anyway", esp_err_to_name(ret));
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect returned %s; wait for connection event anyway", esp_err_to_name(ret));
        }
    }

    return wait_for_wifi_connected(state, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
}

static esp_err_t connect_wifi(photo_cloud_state_t *state)
{
    ESP_RETURN_ON_ERROR(ensure_netif_ready(), TAG, "network init failed");
    s_active_state = state;

    photo_cloud_load_binding_state(state);

    if (state->binding_state_known && !state->bound) {
        ESP_LOGI(TAG, "frame is unbound; keep BLE provisioning active");
        (void)esp_wifi_stop();
        state->wifi_connected = false;
        state->wifi_provisioned = false;
        return start_ble_provisioning(state);
    }

    if (state->wifi_connected || wifi_station_has_link()) {
        state->wifi_connected = true;
        set_status(state, "Wi-Fi 已连接");
        return ESP_OK;
    }

    s_wifi_retry_count = 0;

    if (wifi_has_saved_credentials()) {
        esp_err_t ret = connect_saved_wifi(state);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "saved Wi-Fi unavailable; start BLE provisioning");
        esp_wifi_stop();
        state->wifi_provisioned = false;
        return start_ble_provisioning(state);
    }

    state->wifi_provisioned = false;
    return start_ble_provisioning(state);
}

static esp_err_t make_api_path_url(char *out, size_t out_size, const char *path)
{
    const char *base = CONFIG_PHOTO_API_BASE_URL;
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        len--;
    }

    const char *path_suffix = path;
    if (len >= 4 && strncmp(base + len - 4, "/api", 4) == 0 && strncmp(path, "/api/", 5) == 0) {
        path_suffix = path + 4;
    }

    int written = snprintf(out, out_size, "%.*s%s", (int)len, base, path_suffix);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t make_api_url(char *out, size_t out_size, const char *path, const char *device_uid)
{
    esp_err_t ret = make_api_path_url(out, out_size, path);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = strlen(out);
    int written = snprintf(out + len, out_size - len, "?device_uid=%s", device_uid);
    return written > 0 && (size_t)written < out_size - len ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t http_get_json(const char *url, char *buffer, size_t buffer_size)
{
    int total = 0;
    ESP_LOGI(TAG, "GET JSON %s", url);
    const char *device_secret = device_secret_header_value();
    if (device_secret == NULL) {
        ESP_LOGE(TAG, "device root secret is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "X-Device-Secret", device_secret);

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length >= (int)buffer_size) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    char preview[HTTP_ERROR_PREVIEW_MAX];
    size_t preview_len = 0;
    memset(preview, 0, sizeof(preview));
    int empty_reads = 0;
    while (total < (int)buffer_size - 1) {
        int read_len = esp_http_client_read(client, buffer + total, (int)buffer_size - 1 - total);
        if (read_len < 0) {
            if (read_len == -ESP_ERR_HTTP_EAGAIN && ++empty_reads < HTTP_EMPTY_READ_MAX) {
                vTaskDelay(pdMS_TO_TICKS(HTTP_EMPTY_READ_DELAY_MS));
                continue;
            }
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (++empty_reads >= HTTP_EMPTY_READ_MAX) {
                ESP_LOGW(TAG, "GET stalled before complete: %s errno=%d", url, esp_http_client_get_errno(client));
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTP_EMPTY_READ_DELAY_MS));
            continue;
        }
        empty_reads = 0;
        if (preview_len < sizeof(preview) - 1) {
            size_t preview_remaining = sizeof(preview) - 1 - preview_len;
            size_t preview_bytes = read_len < (int)preview_remaining ? (size_t)read_len : preview_remaining;
            memcpy(preview + preview_len, buffer + total, preview_bytes);
            preview_len += preview_bytes;
            preview[preview_len] = '\0';
        }
        total += read_len;
    }
    buffer[total] = '\0';

    int status_code = esp_http_client_get_status_code(client);
    int http_errno = esp_http_client_get_errno(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed err=%s bytes=%d errno=%d", url, esp_err_to_name(ret), total, http_errno);
        return ret;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "GET %s failed with HTTP %d", url, status_code);
        if (preview_len > 0) {
            for (size_t i = 0; i < preview_len; i++) {
                unsigned char ch = (unsigned char)preview[i];
                if (ch < 0x20 || ch == 0x7F) {
                    preview[i] = ' ';
                }
            }
            ESP_LOGW(TAG, "GET response preview: %s", preview);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "GET JSON ok http=%d bytes=%d", status_code, total);
    return ESP_OK;
}

static esp_err_t make_absolute_media_url(char *out, size_t out_size, const char *media_url)
{
    if (media_url == NULL || media_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(media_url, "http://", 7) == 0 || strncmp(media_url, "https://", 8) == 0) {
        int written = snprintf(out, out_size, "%s", media_url);
        return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_NO_MEM;
    }

    const char *base = CONFIG_PHOTO_API_BASE_URL;
    size_t len = strlen(base);

    // A root-relative URL belongs to the server origin, not to /iot/api.
    if (media_url[0] == '/') {
        const char *scheme = strstr(base, "://");
        if (scheme != NULL) {
            const char *authority_end = strchr(scheme + 3, '/');
            size_t authority_len = authority_end == NULL ? len : (size_t)(authority_end - base);
            int written = snprintf(out, out_size, "%.*s%s", (int)authority_len, base, media_url);
            return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_NO_MEM;
        }
    }

    while (len > 0 && base[len - 1] == '/') {
        len--;
    }
    int written = snprintf(out, out_size, "%.*s/%s", (int)len, base, media_url[0] == '/' ? media_url + 1 : media_url);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool sd_has_room_for_download(const char *album_dir, int content_length)
{
    (void)album_dir;
    if (content_length <= 0) {
        return true;
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(PHOTO_STORAGE_MOUNT_POINT, &total_bytes, &free_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "cannot read SD free space; continue download");
        return true;
    }
    (void)total_bytes;

    uint64_t required = (uint64_t)content_length + PHOTO_SD_MIN_FREE_AFTER_DOWNLOAD;
    if (free_bytes < required) {
        ESP_LOGW(TAG, "skip download because SD is nearly full free=%llu required=%llu",
                 (unsigned long long)free_bytes, (unsigned long long)required);
        return false;
    }
    return true;
}

static esp_err_t http_download_file(const char *url, const char *final_path, const char *album_dir)
{
    char temp_path[MEDIA_PATH_MAX];
    int written = snprintf(temp_path, sizeof(temp_path), "%s/dl.tmp", album_dir);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "download start %s -> %s", url, final_path);
    FILE *file = fopen(temp_path, "wb");
    if (file == NULL) {
        ESP_LOGW(TAG, "open temp file failed: %s", temp_path);
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(file);
        remove(temp_path);
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "download open failed %s err=%s", url, esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        fclose(file);
        remove(temp_path);
        return ret;
    }

    int64_t header_length = esp_http_client_fetch_headers(client);
    int64_t content_length = esp_http_client_get_content_length(client);
    if (header_length < 0) {
        ESP_LOGW(TAG, "download fetch headers failed %s header_len=%lld errno=%d",
                 url, (long long)header_length, esp_http_client_get_errno(client));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(file);
        remove(temp_path);
        return ESP_FAIL;
    }
    if (!sd_has_room_for_download(album_dir,
                                  content_length > INT32_MAX ? INT32_MAX : (int)content_length)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(file);
        remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    char buffer[1024];
    char error_preview[HTTP_ERROR_PREVIEW_MAX];
    size_t error_preview_len = 0;
    memset(error_preview, 0, sizeof(error_preview));
    int total = 0;
    int empty_reads = 0;
    while (true) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len < 0) {
            if (read_len == -ESP_ERR_HTTP_EAGAIN && ++empty_reads < HTTP_EMPTY_READ_MAX) {
                vTaskDelay(pdMS_TO_TICKS(HTTP_EMPTY_READ_DELAY_MS));
                continue;
            }
            ESP_LOGW(TAG, "download read failed %s errno=%d", url, esp_http_client_get_errno(client));
            ret = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (++empty_reads >= HTTP_EMPTY_READ_MAX) {
                ESP_LOGW(TAG, "download stalled before complete %s errno=%d",
                         url, esp_http_client_get_errno(client));
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTP_EMPTY_READ_DELAY_MS));
            continue;
        }
        empty_reads = 0;
        if (error_preview_len < sizeof(error_preview) - 1) {
            size_t preview_remaining = sizeof(error_preview) - 1 - error_preview_len;
            size_t preview_bytes = read_len < (int)preview_remaining
                                       ? (size_t)read_len
                                       : preview_remaining;
            memcpy(error_preview + error_preview_len, buffer, preview_bytes);
            error_preview_len += preview_bytes;
            error_preview[error_preview_len] = '\0';
        }
        if (fwrite(buffer, 1, read_len, file) != (size_t)read_len) {
            ESP_LOGW(TAG, "write temp file failed: %s errno=%d", temp_path, errno);
            ret = ESP_FAIL;
            break;
        }
        total += read_len;
    }

    int status_code = esp_http_client_get_status_code(client);
    bool complete = esp_http_client_is_complete_data_received(client);
    int http_errno = esp_http_client_get_errno(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    fclose(file);

    if (ret != ESP_OK || status_code < 200 || status_code >= 300 || !complete || total <= 0) {
        for (size_t i = 0; i < error_preview_len; i++) {
            unsigned char ch = (unsigned char)error_preview[i];
            if (ch < 0x20 || ch == 0x7F) {
                error_preview[i] = ' ';
            }
        }
        ESP_LOGW(TAG, "download failed http=%d err=%s bytes=%d content_len=%lld header_len=%lld complete=%d errno=%d url=%s",
                 status_code, esp_err_to_name(ret), total, (long long)content_length,
                 (long long)header_length, complete ? 1 : 0, http_errno, url);
        ESP_LOGW(TAG, "download response preview: %s",
                 error_preview_len > 0 ? error_preview : "<empty>");
        remove(temp_path);
        return ESP_FAIL;
    }

    remove(final_path);
    if (rename(temp_path, final_path) != 0) {
        ESP_LOGW(TAG, "rename temp file failed: %s -> %s errno=%d", temp_path, final_path, errno);
        remove(temp_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "download ok http=%d bytes=%d content_len=%lld path=%s",
             status_code, total, (long long)content_length, final_path);
    return ESP_OK;
}

static cJSON *response_data(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed");
        return NULL;
    }
    cJSON *success = cJSON_GetObjectItem(root, "success");
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    bool ok = cJSON_IsTrue(success) || (cJSON_IsNumber(code) && code->valueint == 200);
    if (!ok || data == NULL) {
        ESP_LOGW(TAG, "backend JSON not ok or missing data");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool parse_config(photo_cloud_state_t *state, cJSON *data)
{
    if (state == NULL || data == NULL) {
        return false;
    }

    cJSON *bound = cJSON_GetObjectItem(data, "bound");
    cJSON *frame_id = cJSON_GetObjectItem(data, "frame_id");
    cJSON *config_version = cJSON_GetObjectItem(data, "config_version");
    cJSON *content_version = cJSON_GetObjectItem(data, "content_version");
    cJSON *active_album_id = cJSON_GetObjectItem(data, "active_album_id");
    cJSON *current_media_id = cJSON_GetObjectItem(data, "current_media_id");
    cJSON *brightness = cJSON_GetObjectItem(data, "brightness");
    cJSON *interval = cJSON_GetObjectItem(data, "slideshow_interval_sec");
    cJSON *play_mode = cJSON_GetObjectItem(data, "play_mode");

    if (cJSON_IsFalse(bound)) {
        if (cJSON_IsNumber(config_version) && config_version->valueint > state->config_version) {
            state->config_version = config_version->valueint;
        }
        if (cJSON_IsNumber(content_version) && content_version->valueint > state->content_version) {
            state->content_version = content_version->valueint;
        }
        save_binding_state(state, false);
        state->frame_id[0] = '\0';
        state->active_album_id = 0;
        state->current_media_id = 0;
        state->media_count = 0;
        state->downloaded_count = 0;
        snprintf(state->play_mode, sizeof(state->play_mode), "single");
        photo_cloud_save_cached_state(state);
        set_status(state, "相框未绑定，请在 App 里绑定");
        ESP_LOGI(TAG, "config reports unbound");
        return false;
    }

    if (cJSON_IsString(frame_id)) {
        snprintf(state->frame_id, sizeof(state->frame_id), "%s", frame_id->valuestring);
        if (frame_id->valuestring[0] != '\0') {
            save_binding_state(state, true);
        }
    }
    if (cJSON_IsNumber(config_version)) {
        state->config_version = config_version->valueint;
    }
    if (cJSON_IsNumber(content_version)) {
        state->content_version = content_version->valueint;
    }
    if (cJSON_IsNumber(active_album_id)) {
        state->active_album_id = active_album_id->valueint;
    } else if (cJSON_IsNull(active_album_id)) {
        state->active_album_id = 0;
    }
    if (cJSON_IsNumber(current_media_id)) {
        state->current_media_id = current_media_id->valueint;
    } else if (cJSON_IsNull(current_media_id)) {
        state->current_media_id = 0;
    }
    if (cJSON_IsNumber(brightness)) {
        state->brightness = brightness->valueint;
    }
    if (cJSON_IsNumber(interval)) {
        state->slideshow_interval_sec = interval->valueint;
    }
    if (cJSON_IsString(play_mode) &&
        (strcmp(play_mode->valuestring, "single") == 0 || strcmp(play_mode->valuestring, "slideshow") == 0)) {
        snprintf(state->play_mode, sizeof(state->play_mode), "%s", play_mode->valuestring);
    }
    photo_cloud_save_cached_state(state);
    ESP_LOGI(TAG, "config frame_id=%s config_version=%d content_version=%d active_album_id=%d current_media_id=%d brightness=%d interval=%d play_mode=%s",
             state->frame_id, state->config_version, state->content_version, state->active_album_id,
             state->current_media_id, state->brightness, state->slideshow_interval_sec, state->play_mode);
    return true;
}

static void manifest_add(cloud_media_manifest_t *manifest, int display_order, int media_id)
{
    if (manifest == NULL || media_id <= 0 || display_order < 0 || display_order >= PHOTO_STORAGE_MAX_FILES) {
        return;
    }
    manifest->ids[display_order] = media_id;
    if (display_order + 1 > manifest->count) {
        manifest->count = display_order + 1;
    }
}

static bool manifest_contains(const cloud_media_manifest_t *manifest, int media_id)
{
    if (manifest == NULL || media_id <= 0) {
        return false;
    }
    for (int i = 0; i < manifest->count; i++) {
        if (manifest->ids[i] == media_id) {
            return true;
        }
    }
    return false;
}

static bool find_cached_cloud_file(const char *album_dir, int media_id, char *path, size_t path_size)
{
    DIR *dir = opendir(album_dir);
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int entry_media_id = 0;
        if (!cloud_media_id_from_name(entry->d_name, &entry_media_id) || entry_media_id != media_id) {
            continue;
        }
        int written = snprintf(path, path_size, "%s/%s", album_dir, entry->d_name);
        if (written > 0 && (size_t)written < path_size) {
            struct stat st;
            found = stat(path, &st) == 0 && st.st_size > 0;
        }
        if (found) {
            break;
        }
    }
    closedir(dir);
    return found;
}

static int download_media_items(photo_cloud_state_t *state,
                                cJSON *data,
                                const char *album_dir,
                                int offset,
                                cloud_media_manifest_t *manifest,
                                bool *storage_limited,
                                bool *download_failed)
{
    if (!cJSON_IsArray(data)) {
        return 0;
    }

    mkdir(PHOTO_STORAGE_ALBUM_DIR, 0775);
    mkdir(album_dir, 0775);
    int downloaded = 0;
    int count = cJSON_GetArraySize(data);
    ESP_LOGI(TAG, "media page offset=%d count=%d", offset, count);
    for (int i = 0; i < count && downloaded < PHOTO_STORAGE_MAX_FILES; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (item == NULL) {
            continue;
        }
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *media_type = cJSON_GetObjectItem(item, "media_type");
        cJSON *sort_order = cJSON_GetObjectItem(item, "sort_order");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(url)) {
            ESP_LOGW(TAG, "skip media[%d]: invalid id/url", i);
            continue;
        }
        ESP_LOGI(TAG, "media_list[%d] id=%d sort_order=%d type=%s",
                 i, id->valueint,
                 cJSON_IsNumber(sort_order) ? sort_order->valueint : -1,
                 cJSON_IsString(media_type) ? media_type->valuestring : "");
        if (cJSON_IsString(media_type) && strcmp(media_type->valuestring, "image") != 0) {
            ESP_LOGI(TAG, "skip media id=%d type=%s", id->valueint, media_type->valuestring);
            continue;
        }
        // 缓存文件名序号用后端稳定的 sort_order（而不是列表索引）：
        // 列表索引会在删除中间照片后整体位移，导致所有受影响文件改名并重复下载；
        // sort_order 是后端分配且删除时不重排的稳定值，文件名因此保持不变。
        // 注意：sort_order 需在 make_cloud_filename 的序号上限内（2 位 base36 = 0~1295）。
        int file_order = (cJSON_IsNumber(sort_order) && sort_order->valueint >= 0)
                             ? sort_order->valueint
                             : (offset + i);
        // manifest 只用"列表索引"作数组下标（连续无空洞），仅记录云端存在性与播放顺序，
        // 与文件名序号解耦，避免 sort_order 空洞/累积过大导致越界或被误判为 stale。
        manifest_add(manifest, offset + i, id->valueint);

        char absolute_url[MEDIA_URL_MAX];
        char final_path[MEDIA_PATH_MAX];
        if (make_absolute_media_url(absolute_url, sizeof(absolute_url), url->valuestring) != ESP_OK) {
            ESP_LOGW(TAG, "skip media with long url");
            continue;
        }
        char final_name[13];
        if (!photo_storage_make_cloud_filename(final_name, sizeof(final_name), file_order, id->valueint)) {
            ESP_LOGW(TAG, "skip media id=%d because short filename cannot be made", id->valueint);
            continue;
        }
        int written = snprintf(final_path, sizeof(final_path), "%s/%s", album_dir, final_name);
        if (written <= 0 || (size_t)written >= sizeof(final_path)) {
            ESP_LOGW(TAG, "skip media id=%d with long path", id->valueint);
            continue;
        }

        struct stat st;
        if (stat(final_path, &st) == 0 && st.st_size > 0) {
            ESP_LOGI(TAG, "download skip cached media id=%d path=%s", id->valueint, final_path);
            downloaded++;
            continue;
        }
        char cached_path[MEDIA_PATH_MAX];
        if (find_cached_cloud_file(album_dir, id->valueint, cached_path, sizeof(cached_path))) {
            remove(final_path);
            if (rename(cached_path, final_path) == 0) {
                ESP_LOGI(TAG, "migrate cached media id=%d from=%s to=%s", id->valueint, cached_path, final_path);
                downloaded++;
                continue;
            } else {
                ESP_LOGW(TAG, "migrate cached media failed id=%d from=%s to=%s errno=%d",
                         id->valueint, cached_path, final_path, errno);
            }
        }

        esp_err_t ret = http_download_file(absolute_url, final_path, album_dir);
        if (ret == ESP_OK) {
            downloaded++;
        } else {
            if (ret == ESP_ERR_NO_MEM) {
                if (storage_limited != NULL) {
                    *storage_limited = true;
                }
                if (state != NULL) {
                    set_status(state, "SD 卡空间不足，部分云端照片暂未下载");
                }
            } else if (download_failed != NULL) {
                *download_failed = true;
            }
            ESP_LOGW(TAG, "download failed media id=%d", id->valueint);
        }
    }
    ESP_LOGI(TAG, "media download done downloaded=%d/%d", downloaded, count);
    return downloaded;
}

static bool cloud_media_id_from_name(const char *name, int *media_id)
{
    return photo_storage_cloud_media_id_from_name(name, media_id);
}

static void remove_stale_cloud_files(const cloud_media_manifest_t *manifest, const char *album_dir)
{
    DIR *dir = opendir(album_dir);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int media_id = 0;
        // 缓存文件名已与 media_id 绑定且不再随顺序变化：
        // 只要 media_id 仍在云端列表中，就保留缓存文件（不比较期望文件名），
        // 避免删除/重排导致列表索引位移而误删并重复下载。
        if (cloud_media_id_from_name(entry->d_name, &media_id) &&
            !manifest_contains(manifest, media_id)) {
            char path[MEDIA_PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", album_dir, entry->d_name) > 0) {
                ESP_LOGI(TAG, "remove stale cached media id=%d path=%s", media_id, path);
                remove(path);
            }
        }
    }
    closedir(dir);
}

static bool parse_media_page(cJSON *root, cJSON **items, int *total, bool *has_more)
{
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        *items = data;
        if (total != NULL) {
            *total = cJSON_GetArraySize(data);
        }
        if (has_more != NULL) {
            *has_more = false;
        }
        return true;
    }
    if (!cJSON_IsObject(data)) {
        return false;
    }
    cJSON *page_items = cJSON_GetObjectItem(data, "items");
    if (!cJSON_IsArray(page_items)) {
        return false;
    }
    *items = page_items;
    cJSON *page_total = cJSON_GetObjectItem(data, "total");
    cJSON *page_has_more = cJSON_GetObjectItem(data, "has_more");
    if (total != NULL) {
        *total = cJSON_IsNumber(page_total) ? page_total->valueint : cJSON_GetArraySize(page_items);
    }
    if (has_more != NULL) {
        *has_more = cJSON_IsTrue(page_has_more);
    }
    return true;
}

static esp_err_t sync_media_pages(photo_cloud_state_t *state,
                                  photo_album_t *album,
                                  const char *base_url,
                                  char *response,
                                  size_t response_size)
{
    if (album == NULL || !album->mounted) {
        state->downloaded_count = 0;
        ESP_LOGW(TAG, "SD not mounted; skip media downloads");
        set_status(state, "SD 卡未挂载，无法同步照片");
        return ESP_OK;
    }

    const char *album_dir = photo_storage_album_dir(album);
    cloud_media_manifest_t manifest = {0};
    bool storage_limited = false;
    bool download_failed = false;
    bool full_manifest = true;
    int offset = 0;
    int downloaded_total = 0;
    int reported_total = 0;

    ESP_LOGI(TAG, "cache scope album_id=%d dir=%s", state->active_album_id, album_dir);
    while (offset < PHOTO_STORAGE_MAX_FILES) {
        char page_url[640];
        int written = snprintf(page_url, sizeof(page_url), "%s&limit=%d&offset=%d",
                               base_url, DEVICE_MEDIA_PAGE_LIMIT, offset);
        if (written <= 0 || (size_t)written >= sizeof(page_url)) {
            set_status(state, "照片列表地址过长");
            return ESP_ERR_NO_MEM;
        }

        ESP_RETURN_ON_ERROR(http_get_json(page_url, response, response_size), TAG, "fetch media page failed");

        cJSON *root = response_data(response);
        if (root == NULL) {
            set_status(state, "照片列表解析失败");
            return ESP_FAIL;
        }
        cJSON *items = NULL;
        int total = 0;
        bool has_more = false;
        if (!parse_media_page(root, &items, &total, &has_more)) {
            cJSON_Delete(root);
            set_status(state, "照片列表解析失败");
            return ESP_FAIL;
        }
        int page_count = cJSON_GetArraySize(items);
        reported_total = total;
        downloaded_total += download_media_items(state, items, album_dir, offset, &manifest, &storage_limited, &download_failed);
        cJSON_Delete(root);

        offset += page_count;
        if (!has_more || page_count <= 0 || offset >= total) {
            break;
        }
    }

    if (reported_total > PHOTO_STORAGE_MAX_FILES) {
        full_manifest = false;
        reported_total = PHOTO_STORAGE_MAX_FILES;
        set_status(state, "云端照片超过设备上限，仅同步前 500 张");
    }
    state->media_count = reported_total;
    state->downloaded_count = downloaded_total;
    manifest.total = reported_total;

    /* 只有"完整拉取了全部云端媒体"时才清理本地 stale 文件：
     * - manifest.count == 0(后端返回空列表/接口异常)→ 跳过，防止误删全部缓存
     * - reported_total != manifest.count(分页不全/媒体被跳过)→ 跳过，防止误删
     * 真实删除照片走后端 delete_media / delete_album 命令，不依赖这里兜底。 */
    bool catalog_complete = full_manifest && manifest.count > 0 && reported_total == manifest.count;
    if (catalog_complete && !download_failed && !storage_limited) {
        remove_stale_cloud_files(&manifest, album_dir);
    } else {
        ESP_LOGW(TAG, "keep stale cached media because sync is incomplete or catalog is empty "
                      "(reported=%d manifest=%d)", reported_total, manifest.count);
    }
    if (storage_limited) {
        set_status(state, "SD 卡空间不足，部分云端照片暂未下载");
    } else if (download_failed) {
        set_status(state, "部分照片同步失败，稍后会重试");
    } else if (state->media_count > manifest.count) {
        set_status(state, "照片数量超过设备上限，仅同步前 500 张");
    } else {
        set_status(state, "云端同步完成");
    }
    return ESP_OK;
}

static esp_err_t fetch_config_json(photo_cloud_state_t *state, const char *url, char *response, size_t response_size)
{
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 6; attempt++) {
        ret = http_get_json(url, response, response_size);
        if (ret == ESP_OK || state->bound) {
            return ret;
        }

        set_status(state, "等待 App 完成绑定");
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(state->device_uid, state->status, 0);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return ret;
}

static bool wait_for_frame_binding(photo_cloud_state_t *state, const char *url, char *response, size_t response_size)
{
    int attempts = FRAME_BINDING_WAIT_AFTER_WIFI_MS / FRAME_BINDING_POLL_MS;
    if (attempts < 1) {
        attempts = 1;
    }

    for (int attempt = 0; attempt < attempts; attempt++) {
        set_status(state, "Wi-Fi 已连接，等待 App 完成绑定");
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(state->device_uid, state->status, 0);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_BINDING_POLL_MS));

        esp_err_t ret = http_get_json(url, response, response_size);
        if (ret != ESP_OK) {
            continue;
        }

        cJSON *root = response_data(response);
        if (root == NULL) {
            continue;
        }
        bool bound = parse_config(state, cJSON_GetObjectItem(root, "data"));
        cJSON_Delete(root);
        if (bound) {
            state->wifi_just_provisioned = false;
            return true;
        }
    }

    state->wifi_just_provisioned = false;
    set_status(state, "等待绑定超时，请重新扫码配网");
    return false;
}

esp_err_t photo_cloud_sync_once(photo_cloud_state_t *state, photo_album_t *album)
{
    static char response[HTTP_RESPONSE_MAX];
    char url[512];

    if (s_cloud_sync_mutex == NULL) {
        s_cloud_sync_mutex = xSemaphoreCreateMutex();
        if (s_cloud_sync_mutex == NULL) {
            set_status(state, "同步任务内存不足");
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_cloud_sync_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "cloud sync already running; skip duplicate request");
        set_status(state, "云端同步中");
        return ESP_ERR_INVALID_STATE;
    }

    state->cloud_sync_ready = false;
    esp_err_t ret = connect_wifi(state);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_cloud_sync_mutex);
        return ret;
    }

    if (!state->api_configured) {
        set_status(state, "Wi-Fi 已连接，后端未配置");
        ESP_LOGW(TAG, "PHOTO_API_BASE_URL is empty; skip cloud media download");
        xSemaphoreGive(s_cloud_sync_mutex);
        return ESP_OK;
    }
    if (device_secret_header_value() == NULL) {
        state->setup_blocked = true;
        set_status(state, "设备未写入云端密钥，请联系售后");
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(state->device_uid, state->status, -1);
            lvgl_port_unlock();
        }
        ESP_LOGE(TAG, "PHOTO_DEVICE_SECRET is empty; skip cloud sync");
        xSemaphoreGive(s_cloud_sync_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ret = make_api_url(url, sizeof(url), "/api/device/config", state->device_uid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config url too long");
        xSemaphoreGive(s_cloud_sync_mutex);
        return ret;
    }
    ret = fetch_config_json(state, url, response, sizeof(response));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "fetch config failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_cloud_sync_mutex);
        return ret;
    }

    cJSON *root = response_data(response);
    if (root == NULL) {
        set_status(state, "云端配置解析失败");
        xSemaphoreGive(s_cloud_sync_mutex);
        return ESP_FAIL;
    }
    bool bound = parse_config(state, cJSON_GetObjectItem(root, "data"));
    cJSON_Delete(root);
    if (!bound) {
        if (album != NULL && album->mounted) {
            photo_storage_clear_cloud_cache(album);
        }
        if (state->wifi_just_provisioned &&
            wait_for_frame_binding(state, url, response, sizeof(response))) {
            bound = true;
        }
    }
    if (!bound) {
        if (lvgl_port_lock(100)) {
            photo_ui_show_cloud(state->device_uid, state->status, 0);
            lvgl_port_unlock();
        }
        state->cloud_sync_ready = true;
        xSemaphoreGive(s_cloud_sync_mutex);
        return ESP_OK;
    }

    if (album != NULL && album->mounted) {
        photo_storage_set_album_scope(album, state->active_album_id);
    }

    ret = make_api_url(url, sizeof(url), "/api/device/media", state->device_uid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "media url too long");
        xSemaphoreGive(s_cloud_sync_mutex);
        return ret;
    }
    ret = sync_media_pages(state, album, url, response, sizeof(response));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sync media pages failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_cloud_sync_mutex);
        return ret;
    }

    if (album != NULL && album->mounted) {
        photo_storage_scan(album);
    }

    state->cloud_sync_ready = true;
    xSemaphoreGive(s_cloud_sync_mutex);
    return ESP_OK;
}
