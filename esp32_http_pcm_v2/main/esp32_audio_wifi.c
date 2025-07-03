/**
 * ESP32 Polling-based TTS Audio Player
 * 基于轮询的TTS音频播放系统
 * ESP32-S3-WROOM-1-N16R8
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
#define SAMPLE_RATE            48000        // 使用48kHz，与原项目一致
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1024
#define DMA_BUF_COUNT          8

/* Audio buffer configuration */
#define MAX_AUDIO_SIZE         (256 * 1024)  // 128KB最大音频文件大小
#define DOWNLOAD_CHUNK_SIZE    (4 * 1024)    // 4KB下载块大小
#define POLL_INTERVAL_MS       2000          // 轮询间隔

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
    uint8_t *audio_buffer;
    size_t audio_size;
    size_t audio_capacity;
    size_t audio_position;
    char current_audio_id[64];
} audio_state_t;

static audio_state_t audio_state = {0};

/* HTTP download state */
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} download_state_t;

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

/* HTTP下载事件处理器 */
static esp_err_t download_event_handler(esp_http_client_event_t *evt) {
    download_state_t *download_state = (download_state_t *)evt->user_data;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // 动态扩展缓冲区如果需要
                if (download_state->size + evt->data_len > download_state->capacity) {
                    size_t new_capacity = download_state->capacity + DOWNLOAD_CHUNK_SIZE;
                    if (new_capacity > MAX_AUDIO_SIZE) {
                        ESP_LOGW(TAG, "Audio file too large, truncating");
                        evt->data_len = MAX_AUDIO_SIZE - download_state->size;
                        if (evt->data_len <= 0) {
                            return ESP_OK;
                        }
                    } else {
                        uint8_t *new_buffer = realloc(download_state->buffer, new_capacity);
                        if (!new_buffer) {
                            ESP_LOGE(TAG, "Failed to reallocate download buffer");
                            return ESP_FAIL;
                        }
                        download_state->buffer = new_buffer;
                        download_state->capacity = new_capacity;
                        ESP_LOGD(TAG, "Expanded buffer to %d bytes", new_capacity);
                    }
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
        .size = 0
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

/* 下载PCM音频文件 */
static esp_err_t download_pcm_audio(const char *audio_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s.pcm", TTS_SERVER_URL, audio_id);
    
    ESP_LOGI(TAG, "Downloading PCM: %s", url);
    ESP_LOGI(TAG, "Free heap before download: %d bytes", esp_get_free_heap_size());
    
    // 释放之前的音频缓冲区
    if (audio_state.audio_buffer) {
        free(audio_state.audio_buffer);
        audio_state.audio_buffer = NULL;
    }
    
    // 分配初始缓冲区
    uint8_t *initial_buffer = malloc(DOWNLOAD_CHUNK_SIZE);
    if (!initial_buffer) {
        ESP_LOGE(TAG, "Failed to allocate initial download buffer");
        return ESP_FAIL;
    }
    
    download_state_t download_state = {
        .buffer = initial_buffer,
        .capacity = DOWNLOAD_CHUNK_SIZE,
        .size = 0
    };
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .event_handler = download_event_handler,
        .user_data = &download_state,
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
            strncpy(audio_state.current_audio_id, audio_id, sizeof(audio_state.current_audio_id) - 1);
            
            ESP_LOGI(TAG, "Downloaded %d bytes for audio: %s", download_state.size, audio_id);
            ESP_LOGI(TAG, "Free heap after download: %d bytes", esp_get_free_heap_size());
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

/* I2C初始化 */
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
    
    return i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
}

/* ES8311编解码器初始化 */
static esp_err_t es8311_codec_init(es8311_handle_t *codec_handle) {
    // 启用编解码器电源
    gpio_reset_pin(CODEC_ENABLE_PIN);
    gpio_set_direction(CODEC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    ESP_LOGI(TAG, "ES8311 power enabled on GPIO%d", CODEC_ENABLE_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 启用功率放大器
    gpio_reset_pin(PA_CTRL_PIN);
    gpio_set_direction(PA_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTRL_PIN, 1);
    ESP_LOGI(TAG, "Power amplifier enabled on GPIO%d", PA_CTRL_PIN);
    
    // 创建ES8311句柄
    *codec_handle = es8311_create(I2C_MASTER_NUM, ES8311_I2C_ADDR);
    if (*codec_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 handle");
        return ESP_FAIL;
    }

    // 配置ES8311时钟 - 修复时钟配置
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = false,  // 使用SCLK作为MCLK源
        .mclk_frequency = 0,          // 使用SCLK时忽略此值
        .sample_frequency = SAMPLE_RATE,
    };
    
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311: %s", esp_err_to_name(ret));
        return ret;
    }
    
    es8311_microphone_config(*codec_handle, false);
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    es8311_voice_mute(*codec_handle, false);

    ESP_LOGI(TAG, "ES8311 codec initialized with %dHz sample rate", SAMPLE_RATE);
    return ESP_OK;
}

/* I2S初始化 */
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
            ESP_LOGI(TAG, "Started playing audio: %s (%d bytes)", 
                    audio_state.current_audio_id, audio_state.audio_size);
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
                    ESP_LOGI(TAG, "Audio buffer freed, heap: %d bytes", esp_get_free_heap_size());
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
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        // 如果正在播放音频，等待
        if (audio_state.is_playing) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 清空audio_id缓冲区
        memset(audio_id, 0, sizeof(audio_id));
        
        // 轮询新的TTS任务
        esp_err_t err = poll_for_tts_task(audio_id, sizeof(audio_id));
        
        if (err == ESP_OK && strlen(audio_id) > 0) {
            ESP_LOGI(TAG, "🎵 New TTS task: %s", audio_id);
            
            // 下载PCM音频文件
            esp_err_t download_err = download_pcm_audio(audio_id);
            if (download_err == ESP_OK) {
                ESP_LOGI(TAG, "✅ Audio downloaded successfully: %s", audio_id);
                
                // 等待播放完成
                while (audio_state.is_playing || audio_state.has_audio) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                ESP_LOGI(TAG, "✅ Finished playing: %s", audio_id);
            } else {
                ESP_LOGE(TAG, "❌ Failed to download audio: %s", audio_id);
            }
            
            // 播放完成后短暂延迟
            vTaskDelay(pdMS_TO_TICKS(1000));
            
        } else if (err == ESP_ERR_NOT_FOUND) {
            // 无新任务，正常情况
            ESP_LOGD(TAG, "No new tasks, continuing...");
        } else {
            // 真正的错误，等待后重试
            ESP_LOGW(TAG, "❌ Poll error (%s), retrying in 5 seconds", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 Polling-based TTS Audio Player");
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化WiFi
    ESP_ERROR_CHECK(wifi_init_sta());

    // 初始化I2C
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");

    // 初始化ES8311编解码器
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));

    // 初始化I2S
    ESP_ERROR_CHECK(i2s_init());

    // 创建音频播放任务
    xTaskCreate(audio_playback_task, "audio_playback", 4096, NULL, 10, NULL);

    // 创建TTS轮询任务
    xTaskCreate(tts_polling_task, "tts_polling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready. TTS polling started.");
    ESP_LOGI(TAG, "Server URL: %s", TTS_SERVER_URL);
}