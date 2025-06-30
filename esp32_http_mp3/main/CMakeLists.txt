# esp32_audio/main/CMakeLists.txt

idf_component_register(
    SRCS "es8311_example.c"
    INCLUDE_DIRS "."
    REQUIRES 
        driver
        es8311
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        spiffs
        nvs_flash
        fatfs
)