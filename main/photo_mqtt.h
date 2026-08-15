#ifndef PHOTO_MQTT_H
#define PHOTO_MQTT_H

#include "photo_cloud.h"

void photo_mqtt_attach(photo_cloud_state_t *state, photo_album_t *album);
void photo_mqtt_start(photo_cloud_state_t *state, photo_album_t *album);
void photo_mqtt_show_selected(const char *status);
void photo_mqtt_apply_playback(const char *status);
void photo_mqtt_pause_slideshow(void);

#endif
