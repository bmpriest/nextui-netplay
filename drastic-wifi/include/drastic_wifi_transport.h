#ifndef DRASTIC_WIFI_TRANSPORT_H
#define DRASTIC_WIFI_TRANSPORT_H

#include "drastic_wifi_model.h"

int dwifi_transport_start(struct dwifi_model *model, const char *session_path);
void dwifi_transport_poll(struct dwifi_model *model);
/* Stalls until a multiplayer reply arrives or a short deadline passes. */
void dwifi_transport_await_for_peer(void *opaque);
void dwifi_transport_stop(void);
int dwifi_transport_is_active(void);

#endif
