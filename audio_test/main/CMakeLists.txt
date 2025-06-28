idf_component_register(
                        SRCS "blink_example_main.c"
                        INCLUDE_DIRS "."
                        REQUIRES 
                        esp_codec_dev 
                        driver 
                        esp_timer 
                        freertos
                        esp_pm)
