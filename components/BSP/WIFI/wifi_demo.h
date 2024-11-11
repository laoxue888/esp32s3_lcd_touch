#ifndef WIFI_DEMO_H
#define WIFI_DEMO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#define DEFAULT_WIFI_SSID           "xsf"
#define DEFAULT_WIFI_PASSWORD       "1231936my"

esp_err_t wifi_sta_init(void);

#endif