/**
 * ES8311 Audio Example with WiFi and HTTP Download
 * ESP32-S3-WROOM-1-N16R8
 * Fixed GPIO configuration based on original esp32_audio project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"

#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "es8311.h"

/* WiFi Configuration */
#define WIFI_SSID              "CE-Hub-Student"
#define WIFI_PASSWORD          "casa-ce-gagarin-public-service"
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1
#define WIFI_MAXIMUM_RETRY     5

/* HTTP Configuration */
#define TTS_SERVER_IP          "10.129.113.191"  // Update with your server IP (same network as ESP32)
#define TTS_SERVER_PORT        8001
#define TTS_SERVER_URL         "http://" TTS_SERVER_IP ":8001"

/* Audio Hardware Configuration - FROM ORIGINAL PROJECT */
#define CODEC_ENABLE_PIN       GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN            GPIO_NUM_40  // Power amplifier control pin

/* I2C Configuration - FROM ORIGINAL PROJECT */
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SCL_IO      GPIO_NUM_1  
#define I2C_MASTER_SDA_IO      GPIO_NUM_2   
#define I2C_MASTER_FREQ_HZ     50000        // Changed from 400kHz to 50kHz
#define ES8311_I2C_ADDR        0x18

/* I2S Configuration - FROM ORIGINAL PROJECT */
#define I2S_NUM                I2S_NUM_0
#define I2S_BCK_IO             GPIO_NUM_16  
#define I2S_WS_IO              GPIO_NUM_17  
#define I2S_DO_IO              GPIO_NUM_18  
#define I2S_DI_IO              GPIO_NUM_15  
// Note: MCLK not used in original project

/* Audio Configuration */
#define SAMPLE_RATE            48000        // Changed to 48kHz like original
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1024
#define DMA_BUF_COUNT          8

/* Ring buffer for audio streaming */
#define AUDIO_RING_BUF_SIZE    (32 * 1024)  // 32KB ring buffer

static const char *TAG = "ES8311_HTTP";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static RingbufHandle_t audio_ring_buf = NULL;

/* Audio streaming state */
typedef struct {
    bool is_playing;
    bool stream_done;
    size_t total_received;
    size_t total_played;
} audio_state_t;

static audio_state_t audio_state = {0};

/* WiFi event handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize WiFi */
static esp_err_t wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished.");

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}

/* HTTP streaming event handler */
static esp_err_t http_stream_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
            
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            audio_state.total_received = 0;
            audio_state.stream_done = false;
            break;
            
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            }
            
            // Write PCM data directly to ring buffer
            if (evt->data_len > 0) {
                size_t bytes_written = xRingbufferSend(audio_ring_buf, 
                                                      evt->data, 
                                                      evt->data_len, 
                                                      pdMS_TO_TICKS(1000));
                
                if (bytes_written != evt->data_len) {
                    ESP_LOGW(TAG, "Ring buffer full, wrote %d/%d bytes", 
                            bytes_written, evt->data_len);
                }
                
                audio_state.total_received += bytes_written;
                
                // Start playing after receiving initial data
                if (!audio_state.is_playing && audio_state.total_received > 4096) {
                    audio_state.is_playing = true;
                    ESP_LOGI(TAG, "Started playback after receiving %d bytes", 
                            audio_state.total_received);
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            audio_state.stream_done = true;
            ESP_LOGI(TAG, "Stream complete, received %d bytes", audio_state.total_received);
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/* Stream TTS with optimized API */
static esp_err_t stream_tts_audio(const char* text, const char* voice) {
    char post_data[512];
    
    // Create JSON request body
    snprintf(post_data, sizeof(post_data), 
             "{\"text\":\"%s\",\"voice\":\"%s\"}", text, voice);
    
    ESP_LOGI(TAG, "Requesting TTS stream for: %s", text);
    
    // Configure HTTP client for streaming
    esp_http_client_config_t config = {
        .url = TTS_SERVER_URL "/esp32/pcm",
        .method = HTTP_METHOD_POST,
        .event_handler = http_stream_event_handler,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // Set headers
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    // Reset audio state
    audio_state.is_playing = false;
    audio_state.stream_done = false;
    audio_state.total_received = 0;
    audio_state.total_played = 0;
    
    // Clear ring buffer - consume all items
    size_t item_size;
    void *item = xRingbufferReceive(audio_ring_buf, &item_size, 0);
    while (item != NULL) {
        vRingbufferReturnItem(audio_ring_buf, item);
        item = xRingbufferReceive(audio_ring_buf, &item_size, 0);
    }
    
    // Perform HTTP request (will stream data via event handler)
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d, total_received = %d", 
                status_code, audio_state.total_received);
                
        if (status_code != 200) {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* Audio playback task */
static void audio_playback_task(void *pvParameters) {
    size_t item_size;
    int16_t *audio_data;
    size_t bytes_written;
    
    // Allocate a working buffer for I2S (stereo)
    const size_t i2s_buffer_size = DMA_BUF_LEN * 2 * sizeof(int16_t);  // Stereo
    int16_t *i2s_buffer = (int16_t *)malloc(i2s_buffer_size);
    
    if (!i2s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate I2S buffer");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    while (1) {
        // Wait for audio data in ring buffer
        audio_data = (int16_t *)xRingbufferReceive(audio_ring_buf, 
                                                   &item_size, 
                                                   pdMS_TO_TICKS(100));
        
        if (audio_data != NULL) {
            // Convert mono to stereo if needed
            size_t samples = item_size / sizeof(int16_t);
            for (int i = 0; i < samples && i < DMA_BUF_LEN; i++) {
                i2s_buffer[i * 2] = audio_data[i];      // Left channel
                i2s_buffer[i * 2 + 1] = audio_data[i];  // Right channel
            }
            
            // Return item to ring buffer
            vRingbufferReturnItem(audio_ring_buf, (void *)audio_data);
            
            // Write to I2S
            size_t stereo_size = samples * 2 * sizeof(int16_t);
            esp_err_t ret = i2s_channel_write(tx_handle, i2s_buffer, stereo_size, 
                                            &bytes_written, portMAX_DELAY);
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            } else {
                audio_state.total_played += bytes_written;
            }
            
        } else if (audio_state.stream_done && audio_state.is_playing) {
            // No more data and streaming is done
            ESP_LOGI(TAG, "Playback complete. Played %d bytes", audio_state.total_played);
            audio_state.is_playing = false;
            
            // Small delay before next request
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    
    free(i2s_buffer);
    vTaskDelete(NULL);
}

/* Initialize I2C */
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        return err;
    }

    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

/* Initialize ES8311 codec */
static esp_err_t es8311_codec_init(es8311_handle_t *codec_handle) {
    /* CRITICAL: Enable codec power first! */
    gpio_reset_pin(CODEC_ENABLE_PIN);
    gpio_set_direction(CODEC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    ESP_LOGI(TAG, "ES8311 power enabled on GPIO%d", CODEC_ENABLE_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));  // Wait for power stable
    
    /* Enable power amplifier */
    gpio_reset_pin(PA_CTRL_PIN);
    gpio_set_direction(PA_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTRL_PIN, 1);
    ESP_LOGI(TAG, "Power amplifier enabled on GPIO%d", PA_CTRL_PIN);
    
    /* Create ES8311 handle */
    *codec_handle = es8311_create(I2C_MASTER_NUM, ES8311_I2C_ADDR);
    if (*codec_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 handle");
        return ESP_FAIL;
    }

    /* Configure ES8311 clock - using SCLK as MCLK source like original */
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = false,  // Use SCLK pin as MCLK source
        .mclk_frequency = 0,          // Ignored when using SCLK
        .sample_frequency = SAMPLE_RATE,
    };
    
    /* Initialize ES8311 with 16-bit resolution */
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311");
        return ret;
    }
    
    /* Configure microphone (analog) */
    es8311_microphone_config(*codec_handle, false);
    
    /* Set output volume (0-100) */
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    
    /* Unmute output */
    es8311_voice_mute(*codec_handle, false);

    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

/* Initialize I2S */
static esp_err_t i2s_init(void) {
    /* I2S configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    
    /* Create I2S channel */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    /* Configure I2S standard mode */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // MCLK not connected
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

/* Main TTS request task */
static void tts_request_task(void *pvParameters) {
    const char* test_texts[] = {
        "Hello from ESP32 with streaming audio",
        "This system uses optimized PCM streaming",
        "No files are stored on the device",
        "The audio plays while downloading"
    };
    
    const char* voice = "en-US-AriaNeural";
    int text_index = 0;
    int text_count = sizeof(test_texts) / sizeof(test_texts[0]);
    
    while (1) {
        // Wait if audio is still playing
        while (audio_state.is_playing) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        const char* text = test_texts[text_index];
        ESP_LOGI(TAG, "Requesting TTS: %s", text);
        
        // Stream TTS audio
        if (stream_tts_audio(text, voice) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stream TTS audio");
        }
        
        // Wait for playback to complete
        while (audio_state.is_playing || !audio_state.stream_done) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // Move to next text
        text_index = (text_index + 1) % text_count;
        
        // Wait before next request
        vTaskDelay(pdMS_TO_TICKS(5000));  // 5 seconds between requests
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ES8311 Audio Example with WiFi and HTTP");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Create ring buffer for audio streaming */
    audio_ring_buf = xRingbufferCreate(AUDIO_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (audio_ring_buf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return;
    }

    /* Initialize WiFi */
    ESP_ERROR_CHECK(wifi_init_sta());

    /* Initialize I2C */
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");

    /* Initialize ES8311 codec */
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));

    /* Initialize I2S */
    ESP_ERROR_CHECK(i2s_init());

    /* Create audio playback task with higher priority */
    xTaskCreate(audio_playback_task, "audio_playback", 4096, NULL, 10, NULL);

    /* Create TTS request task */
    xTaskCreate(tts_request_task, "tts_request", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready. Streaming audio system started.");
    ESP_LOGI(TAG, "Server URL: %s", TTS_SERVER_URL);
}