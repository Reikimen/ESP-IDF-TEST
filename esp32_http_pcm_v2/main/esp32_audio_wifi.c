/**
 * ES8311 Audio Example with WiFi and HTTP Polling
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
#define TTS_SERVER_IP          "10.129.113.191"  // Update with your server IP
#define TTS_SERVER_PORT        8001
#define TTS_SERVER_URL         "http://" TTS_SERVER_IP ":8001"
#define DEVICE_ID              "ESP32_VOICE_01"  // Unique device ID

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

/* Audio Configuration */
#define SAMPLE_RATE            48000        // Changed to 48kHz like original
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1024
#define DMA_BUF_COUNT          8

/* Ring buffer for audio streaming */
#define AUDIO_RING_BUF_SIZE    (32 * 1024)  // 32KB ring buffer

static const char *TAG = "ES8311_POLLING";
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
    char current_audio_id[64];
} audio_state_t;

static audio_state_t audio_state = {0};

/* Global buffer for poll response */
static char poll_response_buffer[1024];
static int poll_response_len = 0;

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

/* Initialize WiFi - same as original */
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

/* Global buffer for poll response */
static char poll_response_buffer[1024];
// static int poll_response_len = 0;

/* HTTP event handler for polling */
static esp_err_t poll_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Copy data to our buffer
            if (poll_response_len + evt->data_len < sizeof(poll_response_buffer)) {
                memcpy(poll_response_buffer + poll_response_len, evt->data, evt->data_len);
                poll_response_len += evt->data_len;
            }
            break;
        case HTTP_EVENT_ON_CONNECTED:
            // Reset buffer
            poll_response_len = 0;
            memset(poll_response_buffer, 0, sizeof(poll_response_buffer));
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* Poll for new TTS tasks */
static esp_err_t poll_for_tts_task(char *audio_id, size_t audio_id_size) {
    esp_http_client_config_t config = {
        .url = TTS_SERVER_URL "/esp32/poll",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,  // 30 seconds timeout for long polling
        .buffer_size = 1024,
        .event_handler = poll_event_handler,  // Use event handler to capture response
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // Set device ID header
    esp_http_client_set_header(client, "X-Device-ID", DEVICE_ID);
    
    ESP_LOGI(TAG, "Polling for new tasks...");
    
    // Reset response buffer
    poll_response_len = 0;
    memset(poll_response_buffer, 0, sizeof(poll_response_buffer));
    
    // Perform the request
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "Poll response: status=%d, response_len=%d", status_code, poll_response_len);
        
        if (status_code == 200 && poll_response_len > 0) {
            ESP_LOGI(TAG, "Response: %s", poll_response_buffer);
            
            // Simple JSON parsing to extract audio_id
            char *audio_id_start = strstr(poll_response_buffer, "\"audio_id\":\"");
            if (audio_id_start) {
                audio_id_start += 12;  // Skip to start of audio_id value
                char *audio_id_end = strchr(audio_id_start, '"');
                if (audio_id_end) {
                    size_t id_len = audio_id_end - audio_id_start;
                    if (id_len < audio_id_size - 1) {
                        strncpy(audio_id, audio_id_start, id_len);
                        audio_id[id_len] = '\0';
                        ESP_LOGI(TAG, "New TTS task available: %s", audio_id);
                        err = ESP_OK;
                    } else {
                        ESP_LOGW(TAG, "Audio ID too long: %d", id_len);
                        err = ESP_FAIL;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to find end of audio_id");
                    err = ESP_FAIL;
                }
            } else {
                ESP_LOGW(TAG, "No audio_id found in response");
                err = ESP_FAIL;
            }
        } else if (status_code == 204) {
            // No content - no new tasks, this is normal
            ESP_LOGD(TAG, "No new tasks available (204)");
            err = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGW(TAG, "Unexpected response: status=%d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* HTTP streaming event handler for PCM data */
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

/* Stream audio data for a specific audio_id */
static esp_err_t stream_audio_pcm(const char *audio_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s.pcm", TTS_SERVER_URL, audio_id);
    
    ESP_LOGI(TAG, "Streaming PCM from: %s", url);
    
    // Reset audio state
    audio_state.is_playing = false;
    audio_state.stream_done = false;
    audio_state.total_received = 0;
    audio_state.total_played = 0;
    strncpy(audio_state.current_audio_id, audio_id, sizeof(audio_state.current_audio_id) - 1);
    
    // Clear ring buffer
    size_t item_size;
    void *item = xRingbufferReceive(audio_ring_buf, &item_size, 0);
    while (item != NULL) {
        vRingbufferReturnItem(audio_ring_buf, item);
        item = xRingbufferReceive(audio_ring_buf, &item_size, 0);
    }
    
    // Configure HTTP client for streaming
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_stream_event_handler,
        .timeout_ms = 30000,
        .buffer_size = 2048,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
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

/* Initialize I2C - same as original */
static esp_err_t i2c_master_init(void) {
    int i2c_master_port = I2C_MASTER_NUM;
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    i2c_param_config(i2c_master_port, &conf);
    
    return i2c_driver_install(i2c_master_port, conf.mode,
                            0, 0, 0);
}

/* Initialize ES8311 codec - using correct API */
static esp_err_t es8311_codec_init(es8311_handle_t *codec_handle) {
    /* Enable codec power */
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
            .mclk = I2S_GPIO_UNUSED,
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

/* Audio playback task - same as original */
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
        // Wait for audio data
        audio_data = (int16_t *)xRingbufferReceive(audio_ring_buf, &item_size, portMAX_DELAY);
        
        if (audio_data != NULL && item_size > 0) {
            // Convert mono to stereo
            size_t samples = item_size / sizeof(int16_t);
            size_t stereo_samples = 0;
            
            for (size_t i = 0; i < samples && stereo_samples < DMA_BUF_LEN * 2; i++) {
                i2s_buffer[stereo_samples++] = audio_data[i];  // Left channel
                i2s_buffer[stereo_samples++] = audio_data[i];  // Right channel
            }
            
            // Write to I2S
            size_t bytes_to_write = stereo_samples * sizeof(int16_t);
            i2s_channel_write(tx_handle, i2s_buffer, bytes_to_write, &bytes_written, portMAX_DELAY);
            
            audio_state.total_played += item_size;
            
            // Return item to ring buffer
            vRingbufferReturnItem(audio_ring_buf, (void *)audio_data);
            
            // Check if playback is complete
            if (audio_state.stream_done && audio_state.total_played >= audio_state.total_received) {
                audio_state.is_playing = false;
                ESP_LOGI(TAG, "Playback complete for audio_id: %s", audio_state.current_audio_id);
            }
        }
    }
    
    free(i2s_buffer);
    vTaskDelete(NULL);
}

/* TTS polling task */
static void tts_polling_task(void *pvParameters) {
    char audio_id[64];
    
    ESP_LOGI(TAG, "TTS polling task started, device ID: %s", DEVICE_ID);
    
    // Wait a bit for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        // Wait if audio is still playing
        if (audio_state.is_playing) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Clear audio_id buffer
        memset(audio_id, 0, sizeof(audio_id));
        
        // Poll for new TTS tasks
        esp_err_t err = poll_for_tts_task(audio_id, sizeof(audio_id));
        
        if (err == ESP_OK && strlen(audio_id) > 0) {
            ESP_LOGI(TAG, "🎵 New TTS task received: %s", audio_id);
            
            // Stream the audio PCM data
            esp_err_t stream_err = stream_audio_pcm(audio_id);
            if (stream_err == ESP_OK) {
                ESP_LOGI(TAG, "✅ Successfully started streaming audio: %s", audio_id);
                
                // Wait for playback to complete
                while (audio_state.is_playing || !audio_state.stream_done) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                ESP_LOGI(TAG, "✅ Finished playing audio: %s", audio_id);
            } else {
                ESP_LOGE(TAG, "❌ Failed to stream audio for: %s", audio_id);
            }
            
            // Short delay before next poll
            vTaskDelay(pdMS_TO_TICKS(1000));
            
        } else if (err == ESP_ERR_NOT_FOUND) {
            // No new tasks, this is normal - continue polling immediately
            ESP_LOGD(TAG, "No new TTS tasks, continuing poll...");
            // No delay here - the long polling will handle the wait
        } else {
            // Real error occurred, wait before retrying
            ESP_LOGW(TAG, "❌ Poll error (%s), retrying in 5 seconds", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ES8311 Audio Example with TTS Polling");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    
    /* Set log level for HTTP client debugging */
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);

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

    /* Create TTS polling task */
    xTaskCreate(tts_polling_task, "tts_polling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready. TTS polling started.");
    ESP_LOGI(TAG, "Server URL: %s", TTS_SERVER_URL);
    
    /* Never reached in this example, but for completeness */
    /* es8311_delete(codec_handle); */
}