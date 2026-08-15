#ifndef PHOTO_CLOUD_H
#define PHOTO_CLOUD_H

#include <stdbool.h>
#include "esp_err.h"
#include "photo_storage.h"

#define PHOTO_CLOUD_STATUS_MAX 96
#define PHOTO_CLOUD_DEVICE_UID_MAX 40
#define PHOTO_CLOUD_FRAME_ID_MAX 24
#define PHOTO_CLOUD_PROV_NAME_MAX 16
#define PHOTO_CLOUD_PLAY_MODE_MAX 16

typedef struct {
    char device_uid[PHOTO_CLOUD_DEVICE_UID_MAX];
    char provisioning_name[PHOTO_CLOUD_PROV_NAME_MAX];
    char frame_id[PHOTO_CLOUD_FRAME_ID_MAX];
    char status[PHOTO_CLOUD_STATUS_MAX];
    bool wifi_provisioned;
    bool wifi_connected;
    bool provisioning_active;
    bool provisioning_timed_out;
    bool wifi_just_provisioned;
    bool setup_blocked;
    bool bound;
    bool binding_state_known;
    bool api_configured;
    bool cloud_sync_ready;
    int config_version;
    int content_version;
    int active_album_id;
    int current_media_id;
    int brightness;
    int slideshow_interval_sec;
    char play_mode[PHOTO_CLOUD_PLAY_MODE_MAX];
    int media_count;
    int downloaded_count;
} photo_cloud_state_t;

void photo_cloud_init_state(photo_cloud_state_t *state);
void photo_cloud_load_binding_state(photo_cloud_state_t *state);
void photo_cloud_save_cached_state(photo_cloud_state_t *state);
void photo_cloud_set_bound(photo_cloud_state_t *state, bool bound);
esp_err_t photo_cloud_sync_once(photo_cloud_state_t *state, photo_album_t *album);

#endif
