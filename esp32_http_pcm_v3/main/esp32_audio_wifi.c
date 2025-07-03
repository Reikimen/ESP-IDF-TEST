/**
 * ESP32 Polling-based TTS Audio Player with PSRAM Support
 * 基于轮询的TTS音频播放系统 - 支持PSRAM大文件处理
 * ESP32-S3-WROOM-1-N16R8 (16MB Flash + 8MB PSRAM)
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
#include "esp_heap_caps.h"
#include "esp_psram.h"

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
#define TTS_SERVER_IP          "10.129.113.191"  
#define TTS_SERVER_PORT        8001
#define TTS_SERVER_URL         "http://" TTS_SERVER_IP ":8001"
#define DEVICE_ID              "ESP32_VOICE_01"

/* Audio Hardware Configuration - 保持原有引脚设置 */
#define CODEC_ENABLE_PIN       GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN            GPIO_NUM_40  // Power amplifier control pin

/* I2C Configuration */
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SCL_IO      GPIO_NUM_1  
#define I2C_MASTER_SDA_IO      GPIO_NUM_2   
#define I2C_MASTER_FREQ_HZ     50000
#define ES8311_I2C_ADDR        0x18

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
    
    // 对于大于32KB的分配，尝试使用PSRAM
    if (size > 32 * 1024 && esp_psram_is_initialized()) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (ptr) {
            ESP_LOGI(TAG, "Allocated %d bytes from PSRAM", size);
        }
    }
    
    // 如果PSRAM分配失败或大小较小，使用内部RAM
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ptr) {
            ESP_LOGI(TAG, "Allocated %d bytes from internal RAM", size);
        }
    }
    
    return ptr;
}

/* 内存重分配辅助函数 */
static void* audio_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return audio_malloc(size);
    }
    
    // 对于大内存，尝试在PSRAM中重新分配
    if (size > 32 * 1024 && esp_psram_is_initialized()) {
        void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_ptr) {
            ESP_LOGD(TAG, "Reallocated %d bytes in PSRAM", size);
            return new_ptr;
        }
    }
    
    // 否则使用内部RAM
    void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (new_ptr) {
        ESP_LOGD(TAG, "Reallocated %d bytes in internal RAM", size);
    }
    return new_ptr;
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

/* 初始化WiFi */
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
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

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
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 动态扩展缓冲区如果需要
                if (download_state->size + evt->data_len > download_state->capacity) {
                    size_t new_capacity = download_state->capacity + DOWNLOAD_CHUNK_SIZE;
                    
                    // 检查是否超过最大限制
                    if (new_capacity > MAX_AUDIO_SIZE) {
                        ESP_LOGW(TAG, "Audio file too large (>%d bytes), truncating", MAX_AUDIO_SIZE);
                        evt->data_len = MAX_AUDIO_SIZE - download_state->size;
                        if (evt->data_len <= 0) {
                            return ESP_OK;
                        }
                        new_capacity = MAX_AUDIO_SIZE;
                    }
                    
                    // 使用优化的重分配函数
                    uint8_t *new_buffer = audio_realloc(download_state->buffer, new_capacity);
                    if (!new_buffer) {
                        ESP_LOGE(TAG, "Failed to reallocate download buffer");
                        return ESP_FAIL;
                    }
                    
                    download_state->buffer = new_buffer;
                    download_state->capacity = new_capacity;
                    ESP_LOGD(TAG, "Expanded buffer to %d bytes", new_capacity);
                }
                
                if (evt->data_len > 0) {
                    memcpy(download_state->buffer + download_state->size, evt->data, evt->data_len);
                    download_state->size += evt->data_len;
                }
            }
            break;
        
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP connected for download");
            download_state->size = 0;
            break;
            
        case HTTP_EVENT_ON_HEADER:
            // 可以解析Content-Length头来预分配适当大小的缓冲区
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_length = atoi(evt->header_value);
                ESP_LOGI(TAG, "Content-Length: %d bytes", content_length);
                
                // 如果内容长度已知且合理，预分配整个缓冲区
                if (content_length > 0 && content_length <= MAX_AUDIO_SIZE) {
                    if (content_length > download_state->capacity) {
                        uint8_t *new_buffer = audio_realloc(download_state->buffer, content_length);
                        if (new_buffer) {
                            download_state->buffer = new_buffer;
                            download_state->capacity = content_length;
                            ESP_LOGI(TAG, "Pre-allocated %d bytes for download", content_length);
                        }
                    }
                }
            }
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
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    esp_http_client_set_header(client, "X-Device-ID", DEVICE_ID);
    
    ESP_LOGD(TAG, "Polling for new tasks...");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200 && poll_state.size > 0) {
            poll_buffer[poll_state.size] = '\0';
            ESP_LOGI(TAG, "Poll response: %s", poll_buffer);
            
            // 简单JSON解析提取audio_id
            char *audio_id_start = strstr(poll_buffer, "\"audio_id\":\"");
            if (audio_id_start) {
                audio_id_start += 12;
                char *audio_id_end = strchr(audio_id_start, '"');
                if (audio_id_end) {
                    size_t id_len = audio_id_end - audio_id_start;
                    if (id_len < audio_id_size - 1) {
                        strncpy(audio_id, audio_id_start, id_len);
                        audio_id[id_len] = '\0';
                        ESP_LOGI(TAG, "New TTS task: %s", audio_id);
                        err = ESP_OK;
                    } else {
                        ESP_LOGW(TAG, "Audio ID too long");
                        err = ESP_FAIL;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to parse audio_id");
                    err = ESP_FAIL;
                }
            } else {
                ESP_LOGW(TAG, "No audio_id found in response");
                err = ESP_FAIL;
            }
        } else if (status_code == 204) {
            // 无内容 - 无新任务
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
        .use_psram = (esp_psram_is_initialized() && INITIAL_BUFFER_SIZE > 32 * 1024)
    };
    
    ESP_LOGI(TAG, "Initial buffer allocated in %s", 
             download_state.use_psram ? "PSRAM" : "Internal RAM");
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 60000,  // 增加超时时间以支持大文件
        .event_handler = download_event_handler,
        .user_data = &download_state,
        .buffer_size = 4096,  // 增加HTTP缓冲区大小
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
        .mclk_frequency = 0,          // Ignored when using SCLK as MCLK
        .sample_frequency = SAMPLE_RATE,
    };
    
    /* Initialize ES8311 with 16-bit resolution for both input and output */
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure for analog microphone (not digital) */
    ESP_ERROR_CHECK(es8311_microphone_config(*codec_handle, false));
    
    /* Set voice volume */
    ESP_ERROR_CHECK(es8311_voice_volume_set(*codec_handle, 70, NULL));
    
    /* Unmute the output */
    ESP_ERROR_CHECK(es8311_voice_mute(*codec_handle, false));
    
    ESP_LOGI(TAG, "ES8311 codec initialized successfully at %dHz", SAMPLE_RATE);
    return ESP_OK;
}

/* Initialize I2S */
static esp_err_t i2s_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

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

/* 简单的上采样函数：16kHz -> 48kHz (3倍上采样) */
static void upsample_audio(int16_t *input, size_t input_samples, int16_t *output, size_t *output_samples) {
    *output_samples = 0;
    
    for (size_t i = 0; i < input_samples; i++) {
        // 每个输入样本复制3次
        output[(*output_samples)++] = input[i];
        output[(*output_samples)++] = input[i];
        output[(*output_samples)++] = input[i];
    }
}

/* 音频播放任务 */
static void audio_playback_task(void *pvParameters) {
    size_t bytes_written;
    const size_t chunk_size = DMA_BUF_LEN * 2 * sizeof(int16_t);  // 立体声缓冲区大小
    int16_t *stereo_buffer = malloc(chunk_size);
    int16_t *upsampled_buffer = malloc(DMA_BUF_LEN * 3 * sizeof(int16_t));  // 上采样缓冲区
    
    if (!stereo_buffer || !upsampled_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    while (1) {
        if (audio_state.has_audio && !audio_state.is_playing) {
            // 开始播放
            audio_state.is_playing = true;
            audio_state.audio_position = 0;
            ESP_LOGI(TAG, "Started playing audio: %s (%d bytes from %s)", 
                    audio_state.current_audio_id, audio_state.audio_size,
                    audio_state.use_psram ? "PSRAM" : "Internal RAM");
        }
        
        if (audio_state.is_playing && audio_state.has_audio) {
            // 计算这次要播放的数据量
            size_t remaining = audio_state.audio_size - audio_state.audio_position;
            if (remaining == 0) {
                // 播放完成
                audio_state.is_playing = false;
                audio_state.has_audio = false;
                audio_state.download_complete = false;
                ESP_LOGI(TAG, "Playback complete: %s", audio_state.current_audio_id);
                
                // 释放音频缓冲区
                if (audio_state.audio_buffer) {
                    free(audio_state.audio_buffer);
                    audio_state.audio_buffer = NULL;
                    ESP_LOGI(TAG, "Audio buffer freed");
                    ESP_LOGI(TAG, "Free heap: %d bytes, Free PSRAM: %d bytes", 
                             esp_get_free_heap_size(), 
                             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
                continue;
            }
            
            // 确定这次播放的样本数量（16kHz单声道输入）
            size_t input_chunk_size = (DMA_BUF_LEN / 3) * sizeof(int16_t);  // 考虑3倍上采样
            if (remaining < input_chunk_size) {
                input_chunk_size = remaining;
            }
            
            // 获取输入数据
            int16_t *input_data = (int16_t *)(audio_state.audio_buffer + audio_state.audio_position);
            size_t input_samples = input_chunk_size / sizeof(int16_t);
            
            // 上采样：16kHz -> 48kHz
            size_t upsampled_samples;
            upsample_audio(input_data, input_samples, upsampled_buffer, &upsampled_samples);
            
            // 转换单声道为立体声
            for (size_t i = 0; i < upsampled_samples && i * 2 + 1 < DMA_BUF_LEN * 2; i++) {
                stereo_buffer[i * 2] = upsampled_buffer[i];      // 左声道
                stereo_buffer[i * 2 + 1] = upsampled_buffer[i];  // 右声道
            }
            
            // 写入I2S
            size_t stereo_bytes = upsampled_samples * 2 * sizeof(int16_t);
            if (stereo_bytes > chunk_size) {
                stereo_bytes = chunk_size;
            }
            
            esp_err_t ret = i2s_channel_write(tx_handle, stereo_buffer, stereo_bytes, &bytes_written, portMAX_DELAY);
            
            if (ret == ESP_OK) {
                audio_state.audio_position += input_chunk_size;
            } else {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            // 没有音频时暂停
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    free(stereo_buffer);
    free(upsampled_buffer);
    vTaskDelete(NULL);
}

/* TTS轮询任务 */
static void tts_polling_task(void *pvParameters) {
    char audio_id[64];
    
    ESP_LOGI(TAG, "TTS polling task started, device ID: %s", DEVICE_ID);
    
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    while (1) {
        // 如果当前没有音频在播放或下载，则轮询新任务
        if (!audio_state.has_audio || (!audio_state.is_playing && audio_state.download_complete)) {
            esp_err_t err = poll_for_tts_task(audio_id, sizeof(audio_id));
            
            if (err == ESP_OK) {
                // 有新的TTS任务，下载音频
                err = download_pcm_audio(audio_id);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to download audio: %s", audio_id);
                }
            } else if (err == ESP_ERR_NOT_FOUND) {
                // 没有新任务，继续轮询
                ESP_LOGD(TAG, "No new TTS tasks");
            } else {
                // 轮询失败，等待后重试
                ESP_LOGW(TAG, "Polling failed, will retry");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        
        // 轮询间隔
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    
    vTaskDelete(NULL);
}

/* 系统信息任务 - 监控内存使用 */
static void system_info_task(void *pvParameters) {
    while (1) {
        ESP_LOGI(TAG, "System Info - Free heap: %d bytes, Free PSRAM: %d bytes, Min heap: %d bytes",
                 esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 esp_get_minimum_free_heap_size());
        
        if (audio_state.has_audio) {
            ESP_LOGI(TAG, "Audio buffer: %d bytes in %s, Position: %d/%d",
                     audio_state.audio_capacity,
                     audio_state.use_psram ? "PSRAM" : "Internal RAM",
                     audio_state.audio_position,
                     audio_state.audio_size);
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