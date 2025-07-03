idf_component_register(
    SRCS "esp32_audio_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES driver es8311 esp_wifi nvs_flash esp_http_client esp_psram json lwip
)