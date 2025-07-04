idf_component_register(
    SRCS "main.c"
         "wifi_manager.c"
         "audio_hal.c"
         "http_client.c"
         "audio_player.c"
    INCLUDE_DIRS "."
    REQUIRES driver es8311 esp_wifi nvs_flash esp_http_client spiffs json esp_psram
)