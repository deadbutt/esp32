#include "photo_storage.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/i2c.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#define PHOTO_I2C_MASTER_NUM 0
#define PHOTO_I2C_MASTER_SCL_IO 9
#define PHOTO_I2C_MASTER_SDA_IO 8
#define PHOTO_I2C_MASTER_FREQ_HZ 400000
#define PHOTO_I2C_TIMEOUT_MS 1000

#define PHOTO_SD_MOSI_IO 11
#define PHOTO_SD_MISO_IO 13
#define PHOTO_SD_CLK_IO 12
#define PHOTO_SD_CS_IO -1
#define PHOTO_CH422G_MODE_ADDR 0x24
#define PHOTO_CH422G_OUTPUT_ADDR 0x38
#define PHOTO_CH422G_OUTPUT_SD_ACTIVE 0x0E

static const char *TAG = "photo_storage";

#define PHOTO_STORAGE_MAX_MEDIA_ID 60466175U

static sdmmc_card_t *s_card;
static sdmmc_host_t s_host = SDSPI_HOST_DEFAULT();
static SemaphoreHandle_t s_album_mutex;

void photo_storage_lock(void)
{
    if (s_album_mutex == NULL) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m == NULL) {
            return; /* 极端情况(内存不足)：放弃锁，避免直接卡死 */
        }
        s_album_mutex = m;
    }
    xSemaphoreTake(s_album_mutex, portMAX_DELAY);
}

void photo_storage_unlock(void)
{
    if (s_album_mutex != NULL) {
        xSemaphoreGive(s_album_mutex);
    }
}

static esp_err_t photo_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PHOTO_I2C_MASTER_SDA_IO,
        .scl_io_num = PHOTO_I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PHOTO_I2C_MASTER_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(PHOTO_I2C_MASTER_NUM, &conf), TAG, "i2c param config failed");
    esp_err_t ret = i2c_driver_install(PHOTO_I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "reuse existing i2c bus after install returned %s", esp_err_to_name(ret));
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t photo_select_sd_card(void)
{
    uint8_t write_buf = 0x01;
    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(PHOTO_I2C_MASTER_NUM, PHOTO_CH422G_MODE_ADDR, &write_buf, 1,
                                                   PHOTO_I2C_TIMEOUT_MS / portTICK_PERIOD_MS),
                        TAG, "CH422G mode write failed");

    // Keep the LCD backlight bit high while selecting SD through CH422G.
    write_buf = PHOTO_CH422G_OUTPUT_SD_ACTIVE;
    return i2c_master_write_to_device(PHOTO_I2C_MASTER_NUM, PHOTO_CH422G_OUTPUT_ADDR, &write_buf, 1,
                                      PHOTO_I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static bool has_photo_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }

    return strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0;
}

const char *photo_storage_album_dir(const photo_album_t *album)
{
    if (album == NULL || album->dir[0] == '\0') {
        return PHOTO_STORAGE_ALBUM_DIR;
    }
    return album->dir;
}

void photo_storage_set_album_scope(photo_album_t *album, int active_album_id)
{
    if (album == NULL) {
        return;
    }
    album->active_album_id = active_album_id;
    if (active_album_id > 0) {
        snprintf(album->dir, sizeof(album->dir), PHOTO_STORAGE_ALBUM_DIR "/album_%d", active_album_id);
    } else {
        snprintf(album->dir, sizeof(album->dir), "%s", PHOTO_STORAGE_ALBUM_DIR);
    }
}

static bool encode_base36_fixed(char *out, size_t out_size, unsigned int value, int width)
{
    const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (out == NULL || out_size <= (size_t)width || width <= 0) {
        return false;
    }

    out[width] = '\0';
    for (int i = width - 1; i >= 0; i--) {
        out[i] = digits[value % 36];
        value /= 36;
    }
    return value == 0;
}

static bool decode_base36_fixed(const char *text, int width, unsigned int *out)
{
    if (text == NULL || out == NULL || width <= 0) {
        return false;
    }

    unsigned int value = 0;
    for (int i = 0; i < width; i++) {
        char ch = text[i];
        value *= 36;
        if (ch >= '0' && ch <= '9') {
            value += (unsigned int)(ch - '0');
        } else if (ch >= 'A' && ch <= 'Z') {
            value += (unsigned int)(ch - 'A' + 10);
        } else if (ch >= 'a' && ch <= 'z') {
            value += (unsigned int)(ch - 'a' + 10);
        } else {
            return false;
        }
    }

    *out = value;
    return true;
}

bool photo_storage_make_cloud_filename(char *out, size_t out_size, int display_order, int media_id)
{
    if (out == NULL || out_size == 0 || display_order < 0 || display_order > 1295 ||
        media_id <= 0 || (unsigned int)media_id > PHOTO_STORAGE_MAX_MEDIA_ID) {
        return false;
    }

    char encoded_order[3];
    char encoded_id[6];
    if (!encode_base36_fixed(encoded_order, sizeof(encoded_order), (unsigned int)display_order, 2) ||
        !encode_base36_fixed(encoded_id, sizeof(encoded_id), (unsigned int)media_id, 5)) {
        return false;
    }

    int written = snprintf(out, out_size, "P%s%s.JPG", encoded_order, encoded_id);
    return written > 0 && (size_t)written < out_size;
}

bool photo_storage_make_cloud_path(const photo_album_t *album, int media_id, char *out, size_t out_size)
{
    char filename[13];
    if (out == NULL || out_size == 0 ||
        !photo_storage_make_cloud_filename(filename, sizeof(filename), 0, media_id)) {
        return false;
    }

    const char *album_dir = photo_storage_album_dir(album);
    int written = snprintf(out, out_size, "%s/%s", album_dir, filename);
    return written > 0 && (size_t)written < out_size;
}

bool photo_storage_cloud_media_id_from_name(const char *name, int *media_id)
{
    if (name == NULL || media_id == NULL) {
        return false;
    }

    if (name[0] == 'P' || name[0] == 'p') {
        const char *dot = strchr(name, '.');
        if (dot != NULL && dot == name + 8 && strcasecmp(dot + 1, "JPG") == 0) {
            unsigned int value = 0;
            if (decode_base36_fixed(name + 3, 5, &value) && value > 0) {
                *media_id = (int)value;
                return true;
            }
        }
    }

    if (strncasecmp(name, "cloud_", 6) == 0) {
        int id = 0;
        const char *last_underscore = strrchr(name, '_');
        const char *parse_from = last_underscore != NULL ? last_underscore + 1 : name + 6;
        if (sscanf(parse_from, "%d", &id) == 1 && id > 0) {
            *media_id = id;
            return true;
        }
    }

    return false;
}

bool photo_storage_cloud_media_id_from_path(const char *path, int *media_id)
{
    const char *name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    return photo_storage_cloud_media_id_from_name(name, media_id);
}

bool photo_storage_is_cloud_filename(const char *name)
{
    int media_id = 0;
    return photo_storage_cloud_media_id_from_name(name, &media_id);
}

bool photo_storage_is_cloud_path(const char *path)
{
    int media_id = 0;
    return photo_storage_cloud_media_id_from_path(path, &media_id);
}

static void store_photo_path(photo_album_t *album, const char *name)
{
    const char *album_dir = photo_storage_album_dir(album);
    if (strlen(album_dir) + 1 + strlen(name) >= PHOTO_STORAGE_MAX_PATH) {
        ESP_LOGW(TAG, "skip long photo path: %s", name);
        return;
    }

    size_t dir_len = strlen(album_dir);
    size_t name_len = strlen(name);
    memcpy(album->paths[album->count], album_dir, dir_len);
    album->paths[album->count][dir_len] = '/';
    memcpy(album->paths[album->count] + dir_len + 1, name, name_len);
    album->paths[album->count][dir_len + 1 + name_len] = '\0';

    char *stored_dot = strrchr(album->paths[album->count], '.');
    if (stored_dot != NULL) {
        for (char *p = stored_dot; *p != '\0'; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
    }
    album->count++;
}

static int cloud_media_id_from_path(const char *path)
{
    int id = 0;
    return photo_storage_cloud_media_id_from_path(path, &id) ? id : -1;
}

static int cloud_media_order_from_path(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    int order = 0;
    int id = 0;
    if ((name[0] == 'P' || name[0] == 'p') && photo_storage_cloud_media_id_from_name(name, &id)) {
        unsigned int decoded_order = 0;
        return decode_base36_fixed(name + 1, 2, &decoded_order) ? (int)decoded_order : id;
    }
    if (strncasecmp(name, "cloud_", 6) != 0) {
        return -1;
    }
    const char *last_underscore = strrchr(name, '_');
    if (last_underscore == NULL || last_underscore <= name + 6) {
        return cloud_media_id_from_path(path);
    }
    return sscanf(name + 6, "%d", &order) == 1 ? order : -1;
}

static void clear_cloud_cache_in_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PHOTO_STORAGE_MAX_PATH];
        int written = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (written <= 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            clear_cloud_cache_in_dir(path);
            continue;
        }
        if (has_photo_extension(entry->d_name) && photo_storage_is_cloud_filename(entry->d_name)) {
            ESP_LOGI(TAG, "clear cloud cache file: %s", path);
            remove(path);
        }
    }
    closedir(dir);
}

void photo_storage_clear_cloud_cache(photo_album_t *album)
{
    if (album == NULL || !album->mounted) {
        return;
    }
    clear_cloud_cache_in_dir(PHOTO_STORAGE_ALBUM_DIR);
    photo_storage_set_album_scope(album, 0);
    photo_storage_scan(album);
}

static int compare_photo_paths(const void *left, const void *right)
{
    const char *a = *(const char (*)[PHOTO_STORAGE_MAX_PATH])left;
    const char *b = *(const char (*)[PHOTO_STORAGE_MAX_PATH])right;
    int a_order = cloud_media_order_from_path(a);
    int b_order = cloud_media_order_from_path(b);
    bool a_cloud = a_order >= 0;
    bool b_cloud = b_order >= 0;

    if (a_cloud && !b_cloud) {
        return -1;
    }
    if (!a_cloud && b_cloud) {
        return 1;
    }
    if (a_cloud && b_cloud && a_order != b_order) {
        return a_order < b_order ? -1 : 1;
    }
    return strcasecmp(a, b);
}

static void log_photo_order(const photo_album_t *album)
{
    if (album == NULL) {
        return;
    }
    for (size_t i = 0; i < album->count; i++) {
        ESP_LOGI(TAG, "album_order[%u]=%s media_id=%d",
                 (unsigned)i, album->paths[i], cloud_media_id_from_path(album->paths[i]));
    }
}

esp_err_t photo_storage_mount(photo_album_t *album)
{
    memset(album, 0, sizeof(*album));

    ESP_RETURN_ON_ERROR(photo_i2c_init(), TAG, "i2c init failed");
    ESP_RETURN_ON_ERROR(photo_select_sd_card(), TAG, "sd select failed");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PHOTO_SD_MOSI_IO,
        .miso_io_num = PHOTO_SD_MISO_IO,
        .sclk_io_num = PHOTO_SD_CLK_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(s_host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PHOTO_SD_CS_IO;
    slot_config.host_id = s_host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(PHOTO_STORAGE_MOUNT_POINT, &s_host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    album->mounted = true;
    mkdir(PHOTO_STORAGE_ALBUM_DIR, 0775);
    photo_storage_set_album_scope(album, 0);
    photo_storage_scan(album);
    return ESP_OK;
}

void photo_storage_scan(photo_album_t *album)
{
    photo_storage_lock();
    album->count = 0;

    const char *album_dir = photo_storage_album_dir(album);
    mkdir(PHOTO_STORAGE_ALBUM_DIR, 0775);
    mkdir(album_dir, 0775);

    DIR *dir = opendir(album_dir);
    if (dir == NULL) {
        ESP_LOGW(TAG, "album dir not found: %s", album_dir);
        photo_storage_unlock();
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && album->count < PHOTO_STORAGE_MAX_FILES) {
        if (has_photo_extension(entry->d_name) && photo_storage_is_cloud_filename(entry->d_name)) {
            store_photo_path(album, entry->d_name);
        }
    }

    rewinddir(dir);
    while ((entry = readdir(dir)) != NULL && album->count < PHOTO_STORAGE_MAX_FILES) {
        if (!has_photo_extension(entry->d_name) || photo_storage_is_cloud_filename(entry->d_name)) {
            continue;
        }
        store_photo_path(album, entry->d_name);
    }

    closedir(dir);
    if (album->count > 1) {
        qsort(album->paths, album->count, sizeof(album->paths[0]), compare_photo_paths);
    }
    ESP_LOGI(TAG, "found %u photo file(s)", (unsigned)album->count);
    log_photo_order(album);
    photo_storage_unlock();
}
