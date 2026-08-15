#ifndef PHOTO_UI_H
#define PHOTO_UI_H

#include "photo_storage.h"
#include <stdbool.h>

void photo_ui_show_boot(void);
void photo_ui_show_wifi_reconnect(int timeout_seconds);
void photo_ui_show_provisioning(const char *device_uid, const char *prov_name, const char *pop, const char *status, bool binding_known, bool bound);
void photo_ui_show_album(const photo_album_t *album, const char *status);
void photo_ui_show_local_photo(const char *path, const char *status);
void photo_ui_show_cloud(const char *device_uid, const char *status, int media_count);
void photo_ui_set_brightness(int brightness);

#endif
