#ifndef PHOTO_STORAGE_H
#define PHOTO_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define PHOTO_STORAGE_MOUNT_POINT "/sdcard"
#define PHOTO_STORAGE_ALBUM_DIR PHOTO_STORAGE_MOUNT_POINT "/photo"
#define PHOTO_STORAGE_MAX_FILES 500
#define PHOTO_STORAGE_MAX_PATH 128

typedef struct {
    char paths[PHOTO_STORAGE_MAX_FILES][PHOTO_STORAGE_MAX_PATH];
    char dir[PHOTO_STORAGE_MAX_PATH];
    size_t count;
    int active_album_id;
    bool mounted;
} photo_album_t;

esp_err_t photo_storage_mount(photo_album_t *album);
void photo_storage_set_album_scope(photo_album_t *album, int active_album_id);
const char *photo_storage_album_dir(const photo_album_t *album);
bool photo_storage_make_cloud_filename(char *out, size_t out_size, int display_order, int media_id);
bool photo_storage_make_cloud_path(const photo_album_t *album, int media_id, char *out, size_t out_size);
bool photo_storage_cloud_media_id_from_name(const char *name, int *media_id);
bool photo_storage_cloud_media_id_from_path(const char *path, int *media_id);
bool photo_storage_is_cloud_filename(const char *name);
bool photo_storage_is_cloud_path(const char *path);
void photo_storage_scan(photo_album_t *album);
void photo_storage_clear_cloud_cache(photo_album_t *album);

/* 保护 photo_album_t(count/paths)跨任务访问的互斥锁。
 * photo_storage_scan 内部会加锁；轮播/命令等任务在读取 count/paths 时应持锁。 */
void photo_storage_lock(void);
void photo_storage_unlock(void);

#endif
