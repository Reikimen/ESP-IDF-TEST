#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

/* WiFi Configuration - 保持不变 */
// #define WIFI_SSID              "CE-Hub-Student"
// #define WIFI_PASSWORD          "casa-ce-gagarin-public-service"
#define WIFI_SSID              "CE-Dankao"
#define WIFI_PASSWORD          "CELAB2025"
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1
#define WIFI_MAXIMUM_RETRY     5

/* 初始化WiFi站点模式 */
esp_err_t wifi_init_sta(void);

#endif /* WIFI_MANAGER_H */