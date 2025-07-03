/* ESP32 HTTP PCM Audio Player with WiFi - PSRAM Optimized Version (Fixed)
 * This version uses PSRAM for large audio buffer storage
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_http_client.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "lwip/err.h"
#include "lwip/sys.h"

/* Network Configuration */
#define TTS_SERVER_URL         "http://10.129.113.191:8001"  // 更新为您的服务器地址
#define DEVICE_ID              "ESP32_VOICE_01"

/* WiFi Configuration */
#define WIFI_SSID              "CE-Hub-Student"
#define WIFI_PASSWORD          "casa-ce-gagarin-public-service"
#define WIFI_MAXIMUM_RETRY     10

/* FreeRTOS event group */
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1

/* I2C Configuration for ES8311 */
#define I2C_MASTER_SCL_IO      GPIO_NUM_1  
#define I2C_MASTER_SDA_IO      GPIO_NUM_2  
#define I2C_MASTER_NUM         0
#define I2C_MASTER_FREQ_HZ     50000
#define I2C_MASTER_TIMEOUT_MS  1000

/* ES8311 Configuration */
#define ES8311_ADDR            0x18
#define CODEC_ENABLE_PIN       GPIO_NUM_6
#define PA_CTRL_PIN            GPIO_NUM_40

/* I2S Configuration */
#define I2S_NUM                I2S_NUM_0
#define I2S_BCK_IO             GPIO_NUM_16  
#define I2S_WS_IO              GPIO_NUM_17  
#define I2S_DO_IO              GPIO_NUM_18  
#define I2S_DI_IO              GPIO_NUM_15  

/* Audio Configuration */
#define SAMPLE_RATE            48000        
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1024
#define DMA_BUF_COUNT          8

/* Audio buffer configuration - 增大限制以支持PSRAM */
#define MAX_AUDIO_SIZE         (4 * 1024 * 1024)  // 4MB max audio size
#define DOWNLOAD_CHUNK_SIZE    (32 * 1024)        // 32KB chunks
#define INITIAL_BUFFER_SIZE    (128 * 1024)       // 128KB initial buffer
#define POLL_INTERVAL_MS       2000       

static const char *TAG = "ESP32_POLLING_AUDIO";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

/* Audio playback state */
typedef struct {
    bool is_playing;
    bool has_audio;
    bool download_complete;
    uint8_t *audio_buffer;      // Will be allocated in PSRAM
    size_t audio_size;
    size_t audio_capacity;
    size_t audio_position;
    char current_audio_id[64];
    bool use_psram;             // Flag to indicate PSRAM usage
} audio_state_t;

static audio_state_t audio_state = {0};

/* HTTP download state */
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
    bool use_psram;
} download_state_t;

/* 内存分配辅助函数 - 优先使用PSRAM */
static void* audio_malloc(size_t size) {
    void *ptr = NULL;
    
    // 对于大于16KB的分配，优先使用PSRAM
    if (size > 16 * 1024 && esp_psram_is_initialized()) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr) {
            ESP_LOGI(TAG, "Allocated %d bytes from PSRAM", size);
            return ptr;
        }
    }
    
    // 否则使用内部RAM
    ptr = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    if (ptr) {
        ESP_LOGI(TAG, "Allocated %d bytes from internal RAM", size);
    } else {
        ESP_LOGE(TAG, "Failed to allocate %d bytes", size);
    }
    
    return ptr;
}

/* WiFi事件处理器 */
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

/* WiFi初始化 */
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

    ESP_LOGI(TAG, "WiFi init finished.");

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

/* HTTP下载事件处理器 - 优化内存分配 */
static esp_err_t download_event_handler(esp_http_client_event_t *evt) {
    download_state_t *download_state = (download_state_t *)evt->user_data;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
            
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            download_state->size = 0;
            break;
            
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
            
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
            
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 检查是否需要扩展缓冲区
                while (download_state->size + evt->data_len > download_state->capacity) {
                    size_t new_capacity = download_state->capacity * 2;
                    
                    // 检查是否超过最大限制
                    if (new_capacity > MAX_AUDIO_SIZE) {
                        new_capacity = MAX_AUDIO_SIZE;
                        if (download_state->size + evt->data_len > new_capacity) {
                            ESP_LOGW(TAG, "Audio file too large (>%d bytes), truncating", MAX_AUDIO_SIZE);
                            evt->data_len = new_capacity - download_state->size;
                            if (evt->data_len <= 0) {
                                return ESP_OK;
                            }
                        }
                    }
                    
                    // 分配新的更大的缓冲区
                    uint8_t *new_buffer = audio_malloc(new_capacity);
                    if (!new_buffer) {
                        ESP_LOGE(TAG, "Failed to allocate larger buffer");
                        return ESP_FAIL;
                    }
                    
                    // 复制现有数据到新缓冲区
                    if (download_state->size > 0) {
                        memcpy(new_buffer, download_state->buffer, download_state->size);
                    }
                    
                    // 释放旧缓冲区
                    free(download_state->buffer);
                    
                    // 更新状态
                    download_state->buffer = new_buffer;
                    download_state->capacity = new_capacity;
                    download_state->use_psram = (new_capacity > 16 * 1024 && esp_psram_is_initialized());
                    
                    ESP_LOGI(TAG, "Expanded buffer to %d bytes in %s", 
                             new_capacity, download_state->use_psram ? "PSRAM" : "Internal RAM");
                }
                
                // 复制新数据
                if (evt->data_len > 0) {
                    memcpy(download_state->buffer + download_state->size, evt->data, evt->data_len);
                    download_state->size += evt->data_len;
                    ESP_LOGD(TAG, "Downloaded %d bytes, total: %d", evt->data_len, download_state->size);
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

/* 轮询新的TTS任务 */
static esp_err_t poll_for_tts_task(char *audio_id, size_t audio_id_size) {
    char poll_buffer[1024];
    download_state_t poll_state = {
        .buffer = (uint8_t *)poll_buffer,
        .capacity = sizeof(poll_buffer) - 1,
        .size = 0,
        .use_psram = false
    };
    
    esp_http_client_config_t config = {
        .url = TTS_SERVER_URL "/esp32/poll",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,  // 30秒长轮询
        .event_handler = download_event_handler,
        .user_data = &poll_state,
        .buffer_size = 512,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    // 设置设备ID头
    esp_http_client_set_header(client, "X-Device-ID", DEVICE_ID);
    
    ESP_LOGD(TAG, "Polling for new tasks...");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200 && poll_state.size > 0) {
            // 确保字符串结束
            poll_buffer[poll_state.size] = '\0';
            ESP_LOGD(TAG, "Poll response: %s", poll_buffer);
            
            // 简单解析JSON响应找到audio_id
            char *audio_id_start = strstr(poll_buffer, "\"audio_id\":\"");
            if (audio_id_start) {
                audio_id_start += 12;  // Skip past "audio_id":"
                char *audio_id_end = strchr(audio_id_start, '"');
                if (audio_id_end) {
                    size_t id_len = audio_id_end - audio_id_start;
                    if (id_len < audio_id_size - 1) {
                        strncpy(audio_id, audio_id_start, id_len);
                        audio_id[id_len] = '\0';
                        ESP_LOGI(TAG, "Found new audio task: %s", audio_id);
                    }
                }
            }
        } else if (status_code == 204) {
            ESP_LOGD(TAG, "No new tasks (204)");
            err = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGW(TAG, "Unexpected response: status=%d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP poll failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* 带重试的轮询函数 */
static esp_err_t poll_for_tts_task_with_retry(char *audio_id, size_t audio_id_size) {
    esp_err_t err;
    int retry_count = 0;
    const int max_retries = 3;
    
    while (retry_count < max_retries) {
        err = poll_for_tts_task(audio_id, audio_id_size);
        
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            return err;
        }
        
        retry_count++;
        ESP_LOGW(TAG, "Poll failed, retry %d/%d", retry_count, max_retries);
        vTaskDelay(pdMS_TO_TICKS(1000 * retry_count));  // 递增延迟
    }
    
    return err;
}

/* 下载PCM音频文件 - 使用PSRAM优化 */
static esp_err_t download_pcm_audio(const char *audio_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s.pcm", TTS_SERVER_URL, audio_id);
    
    ESP_LOGI(TAG, "Downloading PCM: %s", url);
    ESP_LOGI(TAG, "Free heap: %d bytes, Free PSRAM: %d bytes", 
             esp_get_free_heap_size(), 
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 释放之前的音频缓冲区
    if (audio_state.audio_buffer) {
        free(audio_state.audio_buffer);
        audio_state.audio_buffer = NULL;
    }
    
    // 分配初始缓冲区（优先使用PSRAM）
    uint8_t *initial_buffer = audio_malloc(INITIAL_BUFFER_SIZE);
    if (!initial_buffer) {
        ESP_LOGE(TAG, "Failed to allocate initial download buffer");
        return ESP_FAIL;
    }
    
    download_state_t download_state = {
        .buffer = initial_buffer,
        .capacity = INITIAL_BUFFER_SIZE,
        .size = 0,
        .use_psram = (esp_psram_is_initialized() && INITIAL_BUFFER_SIZE > 16 * 1024)
    };
    
    ESP_LOGI(TAG, "Initial buffer allocated in %s", 
             download_state.use_psram ? "PSRAM" : "Internal RAM");
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 60000,  // 60秒超时
        .event_handler = download_event_handler,
        .user_data = &download_state,
        .buffer_size = 4096,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(initial_buffer);
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200 && download_state.size > 0) {
            // 成功下载，转移缓冲区所有权给audio_state
            audio_state.audio_buffer = download_state.buffer;
            audio_state.audio_size = download_state.size;
            audio_state.audio_capacity = download_state.capacity;
            audio_state.audio_position = 0;
            audio_state.has_audio = true;
            audio_state.download_complete = true;
            audio_state.use_psram = download_state.use_psram;
            strncpy(audio_state.current_audio_id, audio_id, sizeof(audio_state.current_audio_id) - 1);
            
            ESP_LOGI(TAG, "Downloaded %d bytes for audio: %s (stored in %s)", 
                    download_state.size, audio_id,
                    audio_state.use_psram ? "PSRAM" : "Internal RAM");
            ESP_LOGI(TAG, "Free heap: %d bytes, Free PSRAM: %d bytes", 
                     esp_get_free_heap_size(), 
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "Download failed: status=%d, size=%d", status_code, download_state.size);
            free(download_state.buffer);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP download failed: %s", esp_err_to_name(err));
        free(download_state.buffer);
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* 带重试的下载函数 */
static esp_err_t download_pcm_audio_with_retry(const char *audio_id) {
    esp_err_t err;
    int retry_count = 0;
    const int max_retries = 3;
    
    while (retry_count < max_retries) {
        err = download_pcm_audio(audio_id);
        
        if (err == ESP_OK) {
            return err;
        }
        
        retry_count++;
        ESP_LOGW(TAG, "Download failed, retry %d/%d", retry_count, max_retries);
        vTaskDelay(pdMS_TO_TICKS(2000 * retry_count));  // 递增延迟
    }
    
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
    *codec_handle = es8311_create(I2C_MASTER_NUM, ES8311_ADDR);
    if (!*codec_handle) {
        ESP_LOGE(TAG, "Failed to create ES8311 handle");
        return ESP_FAIL;
    }
    
    /* Configure ES8311 clock - using SCLK as MCLK source */
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = false,  // Use SCLK pin as MCLK source
        .mclk_frequency = 0,          // Ignored when using SCLK
        .sample_frequency = SAMPLE_RATE,
    };
    
    /* Initialize ES8311 with 16-bit resolution for both input and output */
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Configure microphone (analog) */
    es8311_microphone_config(*codec_handle, false);
    
    /* Set output volume (0-100) */
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    
    /* Unmute output */
    es8311_voice_mute(*codec_handle, false);
    
    ESP_LOGI(TAG, "ES8311 codec initialized successfully at %dHz", SAMPLE_RATE);
    return ESP_OK;
}

/* Initialize I2S */
static esp_err_t i2s_init(void) {
    /* I2S channel configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    chan_cfg.auto_clear = true;
    
    /* Create I2S channel */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    
    /* I2S standard configuration */
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
    
    /* Initialize TX channel */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    
    /* Initialize RX channel if needed */
    if (rx_handle) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    }
    
    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

/* 音频播放任务 */
static void audio_playback_task(void *pvParameters) {
    uint8_t *i2s_write_buff = heap_caps_malloc(DMA_BUF_LEN * 2, MALLOC_CAP_DMA);
    if (!i2s_write_buff) {
        ESP_LOGE(TAG, "Failed to allocate I2S write buffer");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    while (1) {
        if (audio_state.has_audio && !audio_state.is_playing) {
            audio_state.is_playing = true;
            audio_state.audio_position = 0;
            ESP_LOGI(TAG, "Starting playback of audio: %s (%d bytes)", 
                     audio_state.current_audio_id, audio_state.audio_size);
        }
        
        if (audio_state.is_playing) {
            size_t bytes_to_play = audio_state.audio_size - audio_state.audio_position;
            if (bytes_to_play > 0) {
                if (bytes_to_play > DMA_BUF_LEN * 2) {
                    bytes_to_play = DMA_BUF_LEN * 2;
                }
                
                // 复制音频数据到DMA缓冲区
                memcpy(i2s_write_buff, 
                       audio_state.audio_buffer + audio_state.audio_position, 
                       bytes_to_play);
                
                // 写入I2S
                size_t bytes_written = 0;
                esp_err_t ret = i2s_channel_write(tx_handle, i2s_write_buff, 
                                                 bytes_to_play, &bytes_written, 
                                                 portMAX_DELAY);
                
                if (ret == ESP_OK) {
                    audio_state.audio_position += bytes_written;
                    
                    // 打印进度（每10%）
                    static int last_progress = -1;
                    int progress = (audio_state.audio_position * 100) / audio_state.audio_size;
                    if (progress / 10 != last_progress / 10) {
                        ESP_LOGI(TAG, "Playback progress: %d%%", progress);
                        last_progress = progress;
                    }
                }
            } else {
                // 播放完成
                ESP_LOGI(TAG, "Playback completed for audio: %s", audio_state.current_audio_id);
                audio_state.is_playing = false;
                audio_state.has_audio = false;
                
                // 释放音频缓冲区
                if (audio_state.audio_buffer) {
                    free(audio_state.audio_buffer);
                    audio_state.audio_buffer = NULL;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* TTS轮询任务 */
static void tts_polling_task(void *pvParameters) {
    ESP_LOGI(TAG, "TTS polling task started, device ID: %s", DEVICE_ID);
    
    while (1) {
        // 如果当前没有音频或已播放完成，则轮询新任务
        if (!audio_state.has_audio) {
            char audio_id[64] = {0};
            esp_err_t err = poll_for_tts_task_with_retry(audio_id, sizeof(audio_id));
            
            if (err == ESP_OK && strlen(audio_id) > 0) {
                ESP_LOGI(TAG, "New TTS task received: %s", audio_id);
                
                // 下载音频
                err = download_pcm_audio_with_retry(audio_id);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to download audio: %s", audio_id);
                }
            } else if (err == ESP_ERR_NOT_FOUND) {
                ESP_LOGD(TAG, "No new tasks available");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* 系统信息任务 */
static void system_info_task(void *pvParameters) {
    ESP_LOGI(TAG, "System Info - Free heap: %d bytes, Free PSRAM: %d bytes, Min heap: %d bytes",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             esp_get_minimum_free_heap_size());
    
    while (1) {
        ESP_LOGI(TAG, "System Info - Free heap: %d bytes, Free PSRAM: %d bytes, Min heap: %d bytes",
                 esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 esp_get_minimum_free_heap_size());
        
        if (audio_state.is_playing) {
            ESP_LOGI(TAG, "Audio playing: %s, progress: %d/%d bytes (buffer in %s)",
                     audio_state.current_audio_id,
                     audio_state.audio_position,
                     audio_state.audio_size,
                     audio_state.use_psram ? "PSRAM" : "Internal RAM");
        }
        
        vTaskDelay(pdMS_TO_TICKS(10000));  // 每10秒打印一次
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 Polling Audio System Starting...");
    
    // 检查PSRAM
    if (esp_psram_is_initialized()) {
        size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "PSRAM initialized, size: %d bytes", psram_size);
    } else {
        ESP_LOGW(TAG, "PSRAM not detected! Large audio files may fail.");
    }
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    ESP_ERROR_CHECK(wifi_init_sta());
    
    // Initialize I2C
    ESP_ERROR_CHECK(i2c_master_init());
    
    // Initialize ES8311 codec
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));
    
    // Initialize I2S
    ESP_ERROR_CHECK(i2s_init());
    
    // 创建音频播放任务
    xTaskCreatePinnedToCore(audio_playback_task, "audio_play", 4096, NULL, 5, NULL, 1);
    
    // 创建TTS轮询任务
    xTaskCreate(tts_polling_task, "tts_poll", 4096, NULL, 5, NULL);
    
    // 创建系统信息任务
    xTaskCreate(system_info_task, "sys_info", 2048, NULL, 1, NULL);
    
    ESP_LOGI(TAG, "System initialized successfully");
}