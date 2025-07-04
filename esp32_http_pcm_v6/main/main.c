/**
 * ESP32 Polling-based TTS Audio Player with PSRAM Support
 * 基于轮询的TTS音频播放系统 - 使用PSRAM支持大文件
 * ESP32-S3-WROOM-1-N16R8
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "wifi_manager.h"
#include "audio_hal.h"
#include "audio_player.h"
#include "http_client.h"

static const char *TAG = "ESP32_POLLING_AUDIO";

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 Polling-based TTS Audio Player with PSRAM Support");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    
    // 检查PSRAM
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM detected: %d bytes total, %d bytes free", 
                psram_size, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGW(TAG, "No PSRAM detected! Large audio files may fail.");
    }
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化WiFi
    ESP_ERROR_CHECK(wifi_init_sta());

    // 初始化音频硬件
    ESP_ERROR_CHECK(audio_hal_init());

    // 初始化音频播放器
    audio_player_init();

    // 创建音频播放任务
    xTaskCreate(audio_playback_task, "audio_playback", 4096, NULL, 10, NULL);

    // 创建TTS轮询任务
    xTaskCreate(tts_polling_task, "tts_polling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready. TTS polling started.");
    ESP_LOGI(TAG, "Server URL: %s", TTS_SERVER_URL);
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
}