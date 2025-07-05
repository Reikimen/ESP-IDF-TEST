/**
 * ESP32 Polling-based TTS Audio Player with PSRAM Support
 * 基于轮询的TTS音频播放系统 - 使用PSRAM支持大文件
 * ESP32-S3-WROOM-1-N16R8
 * 
 * V6版本新增：麦克风持续收音功能，支持语音活动检测(VAD)和STT服务集成
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
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
#include "esp_heap_caps.h"  // 用于PSRAM分配

#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "es8311.h"

/* WiFi Configuration - 保持不变 */
// #define WIFI_SSID              "CE-Hub-Student"
// #define WIFI_PASSWORD          "casa-ce-gagarin-public-service"
#define WIFI_SSID              "CE-Dankao"
#define WIFI_PASSWORD          "CELAB2025"
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1
#define WIFI_MAXIMUM_RETRY     5

/* HTTP Configuration - 保持不变 */
// #define TTS_SERVER_IP          "10.129.113.191"
#define TTS_SERVER_IP          "192.168.32.177"
#define TTS_SERVER_PORT        8001
#define TTS_SERVER_URL         "http://" TTS_SERVER_IP ":8001"
#define STT_SERVER_PORT        8000
#define STT_SERVER_URL         "http://" TTS_SERVER_IP ":8000"
#define DEVICE_ID              "ESP32_VOICE_01"

/* Audio Hardware Configuration - 保持原有引脚设置 */
#define CODEC_ENABLE_PIN       GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN            GPIO_NUM_40  // Power amplifier control pin

/* I2C Configuration - 保持不变 */
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SCL_IO      GPIO_NUM_1  
#define I2C_MASTER_SDA_IO      GPIO_NUM_2   
#define I2C_MASTER_FREQ_HZ     50000
#define ES8311_I2C_ADDR        0x18

/* I2S Configuration - 保持不变 */
#define I2S_NUM                I2S_NUM_0
#define I2S_BCK_IO             GPIO_NUM_16  
#define I2S_WS_IO              GPIO_NUM_17  
#define I2S_DO_IO              GPIO_NUM_18  
#define I2S_DI_IO              GPIO_NUM_15  

/* Audio Configuration */
#define SAMPLE_RATE            48000        // 使用48kHz，与原项目一致
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1023         // 修改为1023以避免DMA警告
#define DMA_BUF_COUNT          8

/* Audio buffer configuration - 使用PSRAM后可以增大缓冲区 */
#define MAX_AUDIO_SIZE         (4 * 1024 * 1024)  // 增大到4MB
#define DOWNLOAD_CHUNK_SIZE    (64 * 1024)        // 增大到64KB
#define POLL_INTERVAL_MS       2000       

/* Microphone Recording Configuration - 新增麦克风配置 */
#define MIC_SAMPLE_RATE        16000        // STT服务通常使用16kHz
#define MIC_RECORDING_SIZE     (1024 * 1024) // 1MB recording buffer in PSRAM
#define MIC_CHUNK_SIZE         (1024 * 4)   // 4KB chunks
#define VOICE_THRESHOLD        500          // 音量阈值
#define SILENCE_DURATION_MS    3000         // 静音持续时间
#define MIN_RECORDING_MS       500          // 最小录音时长

static const char *TAG = "ESP32_POLLING_AUDIO";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static es8311_handle_t codec_handle = NULL;  // 全局编解码器句柄

/* Audio playback state */
typedef struct {
    bool is_playing;
    bool has_audio;
    bool download_complete;
    uint8_t *audio_buffer;      // 将使用PSRAM分配
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

/* Microphone recording state - 新增麦克风录音状态 */
typedef struct {
    bool is_recording;          // 是否正在录音
    bool voice_detected;        // 是否检测到声音
    uint8_t *recording_buffer;  // 录音缓冲区（PSRAM）
    size_t recording_size;      // 当前录音大小
    size_t recording_capacity;  // 录音缓冲区容量
    int silence_counter;        // 静音计数器
    int recording_duration;     // 录音时长（毫秒）
} mic_state_t;

static mic_state_t mic_state = {0};

/* 函数声明 - 解决编译顺序问题 */
static esp_err_t wifi_init_sta(void);
static esp_err_t download_event_handler(esp_http_client_event_t *evt);
static esp_err_t poll_for_tts_task(char *audio_id, size_t audio_id_size);
static esp_err_t download_pcm_audio(const char *audio_id);
static esp_err_t upload_recording_to_stt(uint8_t *recording_data, size_t recording_size);
static esp_err_t i2c_master_init(void);
static esp_err_t es8311_codec_init(es8311_handle_t *codec_handle);
static esp_err_t i2s_init(void);
static void upsample_audio(int16_t *input, size_t input_samples, int16_t *output, size_t *output_samples);
static void downsample_audio(int16_t *input, size_t input_samples, int16_t *output, size_t *output_samples);
static int calculate_volume(int16_t *samples, size_t num_samples);
static void audio_playback_task(void *pvParameters);
static void microphone_recording_task(void *pvParameters);
static void tts_polling_task(void *pvParameters);

/* 使用PSRAM分配内存的辅助函数 */
static void* psram_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed, falling back to internal RAM");
        ptr = malloc(size);
    }
    return ptr;
}

static void* psram_realloc(void *ptr, size_t size) {
    void *new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (!new_ptr) {
        ESP_LOGW(TAG, "PSRAM reallocation failed, trying internal RAM");
        new_ptr = realloc(ptr, size);
    }
    return new_ptr;
}

/* WiFi事件处理器 - 保持不变 */
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

/* WiFi初始化 - 保持不变 */
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

/* HTTP下载事件处理器 - 修改为使用PSRAM */
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
                        uint8_t *new_buffer = psram_realloc(download_state->buffer, new_capacity);
                        if (!new_buffer) {
                            ESP_LOGE(TAG, "Failed to reallocate download buffer");
                            return ESP_FAIL;
                        }
                        download_state->buffer = new_buffer;
                        download_state->capacity = new_capacity;
                        ESP_LOGD(TAG, "Expanded buffer to %d bytes in PSRAM", new_capacity);
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

/* 轮询TTS任务 - 保持HTTP API调用不变 */
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

/* 下载PCM音频文件 - 修改为使用PSRAM */
static esp_err_t download_pcm_audio(const char *audio_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s.pcm", TTS_SERVER_URL, audio_id);
    
    ESP_LOGI(TAG, "📥 Downloading PCM: %s", url);
    ESP_LOGI(TAG, "Free heap before download: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 释放之前的音频缓冲区
    if (audio_state.audio_buffer) {
        free(audio_state.audio_buffer);
        audio_state.audio_buffer = NULL;
        audio_state.has_audio = false;  // 确保状态清除
    }
    
    // 在PSRAM中分配初始缓冲区
    uint8_t *initial_buffer = psram_malloc(DOWNLOAD_CHUNK_SIZE);
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
    
    ESP_LOGI(TAG, "Starting download...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Download complete. Status: %d, Size: %d bytes", status_code, download_state.size);
        
        if (status_code == 200 && download_state.size > 0) {
            // 成功下载，转移缓冲区所有权给audio_state
            audio_state.audio_buffer = download_state.buffer;
            audio_state.audio_size = download_state.size;
            audio_state.audio_capacity = download_state.capacity;
            audio_state.audio_position = 0;
            audio_state.has_audio = true;
            audio_state.download_complete = true;
            strncpy(audio_state.current_audio_id, audio_id, sizeof(audio_state.current_audio_id) - 1);
            
            ESP_LOGI(TAG, "✅ Downloaded %d bytes for audio: %s", download_state.size, audio_id);
            ESP_LOGI(TAG, "Free heap after download: %d bytes", esp_get_free_heap_size());
            ESP_LOGI(TAG, "Free PSRAM after download: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "Audio state - has_audio: %d, download_complete: %d", 
                    audio_state.has_audio, audio_state.download_complete);
        } else {
            ESP_LOGW(TAG, "❌ Download failed: status=%d, size=%d", status_code, download_state.size);
            free(download_state.buffer);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "❌ HTTP download failed: %s", esp_err_to_name(err));
        free(download_state.buffer);
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* 上传录音到STT服务 - 修改版本 */
static esp_err_t upload_recording_to_stt(uint8_t *recording_data, size_t recording_size) {
    char url[256];
    snprintf(url, sizeof(url), "%s/upload_pcm", STT_SERVER_URL);
    
    ESP_LOGI(TAG, "Uploading PCM recording to STT: %d bytes", recording_size);
    ESP_LOGI(TAG, "STT URL: %s", url);
    ESP_LOGI(TAG, "Device ID: %s", DEVICE_ID);
    
    // 创建multipart/form-data
    char boundary[] = "----ESP32FormBoundary";
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    
    // 获取时间戳
    time_t upload_timestamp = time(NULL);
    
    // 构建multipart body
    // 1. 添加device_id字段（可选但推荐）
    char device_field[256];
    snprintf(device_field, sizeof(device_field),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n"
        "%s\r\n",
        boundary, DEVICE_ID);
    
    // 2. 添加文件字段，确保文件名格式正确
    char file_field[512];
    snprintf(file_field, sizeof(file_field),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"esp32_%s_%ld.pcm\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, DEVICE_ID, (long)upload_timestamp);
    
    char footer[128];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);
    
    // 计算总大小
    size_t total_size = strlen(device_field) + strlen(file_field) + recording_size + strlen(footer);
    
    ESP_LOGI(TAG, "Multipart total size: %d bytes", total_size);
    ESP_LOGI(TAG, "Filename: esp32_%s_%ld.pcm", DEVICE_ID, (long)upload_timestamp);
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,  // 增加缓冲区大小
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for STT upload");
        return ESP_FAIL;
    }
    
    // 设置headers
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "X-Device-ID", DEVICE_ID);  // 额外添加header作为备份
    
    // 打开连接
    esp_err_t err = esp_http_client_open(client, total_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP client: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    
    // 发送device_id字段
    int wlen = esp_http_client_write(client, device_field, strlen(device_field));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write device_id field");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    // 发送文件字段header
    wlen = esp_http_client_write(client, file_field, strlen(file_field));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Failed to write file field header");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    // 发送PCM数据
    size_t uploaded = 0;
    size_t chunk_size = 4096;
    while (uploaded < recording_size && err == ESP_OK) {
        size_t to_write = (recording_size - uploaded) > chunk_size ? chunk_size : (recording_size - uploaded);
        wlen = esp_http_client_write(client, (char *)(recording_data + uploaded), to_write);
        if (wlen <= 0) {
            ESP_LOGE(TAG, "Failed to write PCM data at offset %d", uploaded);
            err = ESP_FAIL;
            break;
        }
        uploaded += wlen;
        
        // 打印上传进度
        if (uploaded % (chunk_size * 10) == 0 || uploaded == recording_size) {
            ESP_LOGI(TAG, "Uploaded %d/%d bytes (%.1f%%)", 
                    uploaded, recording_size, (float)uploaded * 100 / recording_size);
        }
        
        // 添加看门狗喂狗
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // 发送multipart footer
    if (err == ESP_OK) {
        wlen = esp_http_client_write(client, footer, strlen(footer));
        if (wlen < 0) {
            ESP_LOGE(TAG, "Failed to write multipart footer");
            err = ESP_FAIL;
        }
    }
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Upload complete, waiting for response...");
        
        // 获取响应
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "STT response - Status: %d, Content-Length: %d", status_code, content_length);
        
        if (status_code == 200) {
            ESP_LOGI(TAG, "✅ STT upload successful");
            
            // 读取响应
            if (content_length > 0 && content_length < 4096) {
                char *response = malloc(content_length + 1);
                if (response) {
                    int read_len = esp_http_client_read(client, response, content_length);
                    if (read_len > 0) {
                        response[read_len] = '\0';
                        ESP_LOGI(TAG, "STT response: %s", response);
                        
                        // 解析JSON获取转录文本
                        char *text_start = strstr(response, "\"text\":\"");
                        if (text_start) {
                            text_start += 8;
                            char *text_end = strchr(text_start, '"');
                            if (text_end) {
                                *text_end = '\0';
                                ESP_LOGI(TAG, "📝 Transcribed: \"%s\"", text_start);
                            }
                        }
                        
                        // 解析返回的device_id（用于验证）
                        char *device_start = strstr(response, "\"device_id\":\"");
                        if (device_start) {
                            device_start += 13;
                            char *device_end = strchr(device_start, '"');
                            if (device_end) {
                                char returned_device[64];
                                size_t device_len = device_end - device_start;
                                if (device_len < sizeof(returned_device) - 1) {
                                    strncpy(returned_device, device_start, device_len);
                                    returned_device[device_len] = '\0';
                                    ESP_LOGI(TAG, "✅ Confirmed device_id: %s", returned_device);
                                }
                            }
                        }
                    }
                    free(response);
                }
            }
        } else {
            ESP_LOGW(TAG, "❌ STT upload failed with status: %d", status_code);
            
            // 尝试读取错误响应
            char error_buffer[512];
            int read_len = esp_http_client_read(client, error_buffer, sizeof(error_buffer) - 1);
            if (read_len > 0) {
                error_buffer[read_len] = '\0';
                ESP_LOGE(TAG, "Error response: %s", error_buffer);
            }
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Upload failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    return err;
}

/* I2C初始化 - 保持不变 */
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

/* ES8311编解码器初始化 - 保持不变 */
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

    // 配置ES8311时钟
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
    
    // 配置麦克风（模拟输入）
    es8311_microphone_config(*codec_handle, false);
    
    // 设置麦克风增益
    es8311_microphone_gain_set(*codec_handle, ES8311_MIC_GAIN_18DB);
    
    // 设置输出音量
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    
    // 确保输出未静音
    es8311_voice_mute(*codec_handle, false);

    ESP_LOGI(TAG, "ES8311 codec initialized with %dHz sample rate", SAMPLE_RATE);
    return ESP_OK;
}

/* I2S初始化 - 保持不变 */
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

/* 简单的上采样函数：16kHz -> 48kHz (3倍上采样) - 保持不变 */
static void upsample_audio(int16_t *input, size_t input_samples, int16_t *output, size_t *output_samples) {
    *output_samples = 0;
    
    for (size_t i = 0; i < input_samples; i++) {
        // 每个输入样本复制3次
        output[(*output_samples)++] = input[i];
        output[(*output_samples)++] = input[i];
        output[(*output_samples)++] = input[i];
    }
}

/* 简单的下采样函数：48kHz -> 16kHz (3倍下采样) - 新增函数 */
static void downsample_audio(int16_t *input, size_t input_samples, int16_t *output, size_t *output_samples) {
    *output_samples = 0;
    
    for (size_t i = 0; i < input_samples; i += 3) {
        // 每3个样本取1个（简单抽取）
        output[(*output_samples)++] = input[i];
    }
}

/* 计算音频块的平均音量 - 新增函数 */
static int calculate_volume(int16_t *samples, size_t num_samples) {
    int64_t sum = 0;
    for (size_t i = 0; i < num_samples; i++) {
        sum += abs(samples[i]);
    }
    return (int)(sum / num_samples);
}

/* 音频播放任务 - 播放缓冲区使用内部RAM以保证性能 */
static void audio_playback_task(void *pvParameters) {
    size_t bytes_written;
    const size_t chunk_size = DMA_BUF_LEN * 2 * sizeof(int16_t);  // 立体声缓冲区大小
    int16_t *stereo_buffer = malloc(chunk_size);  // 使用内部RAM以保证I2S性能
    int16_t *upsampled_buffer = malloc(DMA_BUF_LEN * 3 * sizeof(int16_t));  // 上采样缓冲区
    
    if (!stereo_buffer || !upsampled_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    int play_counter = 0;  // 用于调试
    
    while (1) {
        if (audio_state.has_audio && !audio_state.is_playing) {
            // 开始播放
            audio_state.is_playing = true;
            audio_state.audio_position = 0;
            play_counter = 0;
            ESP_LOGI(TAG, "🔊 Started playing audio: %s (%d bytes)", 
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
                ESP_LOGI(TAG, "✅ Playback complete: %s (played %d chunks)", 
                        audio_state.current_audio_id, play_counter);
                
                // 释放音频缓冲区
                if (audio_state.audio_buffer) {
                    free(audio_state.audio_buffer);
                    audio_state.audio_buffer = NULL;
                    ESP_LOGI(TAG, "Audio buffer freed, heap: %d bytes", esp_get_free_heap_size());
                    ESP_LOGI(TAG, "PSRAM free: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
                continue;
            }
            
            // 确定这次播放的样本数量（16kHz单声道输入）
            size_t input_chunk_size = (DMA_BUF_LEN / 3) * sizeof(int16_t);  // 考虑3倍上采样
            if (remaining < input_chunk_size) {
                input_chunk_size = remaining;
            }
            
            // 获取输入数据（从PSRAM）
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
                play_counter++;
                
                // 每秒打印一次进度
                if (play_counter % 40 == 0) {  // 约每秒（48000Hz / 1024 samples ≈ 47 chunks/sec）
                    int percent = (audio_state.audio_position * 100) / audio_state.audio_size;
                    ESP_LOGI(TAG, "Playing... %d%% (%d/%d bytes)", 
                            percent, audio_state.audio_position, audio_state.audio_size);
                }
            } else {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            // 没有音频播放时的短暂延时
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    
    free(stereo_buffer);
    free(upsampled_buffer);
    vTaskDelete(NULL);
}

/* 麦克风录音任务 - 新增任务 */
static void microphone_recording_task(void *pvParameters) {
    size_t bytes_read;
    const size_t chunk_size = MIC_CHUNK_SIZE;
    int16_t *stereo_buffer = malloc(chunk_size);  // 立体声输入缓冲区
    int16_t *mono_buffer = malloc(chunk_size / 2); // 单声道缓冲区
    int16_t *downsampled_buffer = malloc(chunk_size / 6); // 下采样后的缓冲区
    
    if (!stereo_buffer || !mono_buffer || !downsampled_buffer) {
        ESP_LOGE(TAG, "Failed to allocate microphone buffers");
        vTaskDelete(NULL);
        return;
    }
    
    // 在PSRAM中分配录音缓冲区
    mic_state.recording_buffer = psram_malloc(MIC_RECORDING_SIZE);
    if (!mic_state.recording_buffer) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer in PSRAM");
        free(stereo_buffer);
        free(mono_buffer);
        free(downsampled_buffer);
        vTaskDelete(NULL);
        return;
    }
    mic_state.recording_capacity = MIC_RECORDING_SIZE;
    
    ESP_LOGI(TAG, "Microphone recording task started");
    ESP_LOGI(TAG, "Voice threshold: %d, Silence duration: %dms", VOICE_THRESHOLD, SILENCE_DURATION_MS);
    
    int sample_counter = 0;
    
    while (1) {
        // 如果正在播放音频，暂停录音
        if (audio_state.is_playing) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 从I2S读取音频数据
        esp_err_t ret = i2s_channel_read(rx_handle, stereo_buffer, chunk_size, &bytes_read, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK && bytes_read > 0) {
            size_t stereo_samples = bytes_read / sizeof(int16_t);
            size_t mono_samples = stereo_samples / 2;
            
            // 转换立体声到单声道（取左声道）
            for (size_t i = 0; i < mono_samples; i++) {
                mono_buffer[i] = stereo_buffer[i * 2];
            }
            
            // 下采样：48kHz -> 16kHz
            size_t downsampled_samples;
            downsample_audio(mono_buffer, mono_samples, downsampled_buffer, &downsampled_samples);
            
            // 计算音量
            int volume = calculate_volume(downsampled_buffer, downsampled_samples);
            
            // 语音活动检测（VAD）
            if (volume > VOICE_THRESHOLD) {
                if (!mic_state.is_recording) {
                    // 开始录音
                    mic_state.is_recording = true;
                    mic_state.voice_detected = true;
                    mic_state.recording_size = 0;
                    mic_state.silence_counter = 0;
                    mic_state.recording_duration = 0;
                    ESP_LOGI(TAG, "Voice detected, start recording (volume: %d)", volume);
                }
                
                // 重置静音计数器
                mic_state.silence_counter = 0;
                
                // 将数据写入录音缓冲区
                if (mic_state.recording_size + downsampled_samples * sizeof(int16_t) < mic_state.recording_capacity) {
                    memcpy(mic_state.recording_buffer + mic_state.recording_size, 
                           downsampled_buffer, 
                           downsampled_samples * sizeof(int16_t));
                    mic_state.recording_size += downsampled_samples * sizeof(int16_t);
                }
            } else if (mic_state.is_recording) {
                // 静音期间
                mic_state.silence_counter += (downsampled_samples * 1000) / MIC_SAMPLE_RATE;
                
                // 继续记录静音数据
                if (mic_state.recording_size + downsampled_samples * sizeof(int16_t) < mic_state.recording_capacity) {
                    memcpy(mic_state.recording_buffer + mic_state.recording_size, 
                           downsampled_buffer, 
                           downsampled_samples * sizeof(int16_t));
                    mic_state.recording_size += downsampled_samples * sizeof(int16_t);
                }
                
                // 检查是否超过静音阈值
                if (mic_state.silence_counter >= SILENCE_DURATION_MS) {
                    // 停止录音
                    mic_state.is_recording = false;
                    mic_state.voice_detected = false;
                    
                    // 计算录音时长
                    mic_state.recording_duration = (mic_state.recording_size / sizeof(int16_t)) * 1000 / MIC_SAMPLE_RATE;
                    
                    ESP_LOGI(TAG, "Recording stopped (silence), duration: %dms, size: %d bytes", 
                            mic_state.recording_duration, mic_state.recording_size);
                    
                    // 如果录音时长足够，上传到STT服务
                    if (mic_state.recording_duration >= MIN_RECORDING_MS) {
                        upload_recording_to_stt(mic_state.recording_buffer, mic_state.recording_size);
                    } else {
                        ESP_LOGW(TAG, "Recording too short, discarding");
                    }
                    
                    // 重置状态
                    mic_state.recording_size = 0;
                    mic_state.silence_counter = 0;
                }
            }
            
            // 更新录音时长
            if (mic_state.is_recording) {
                mic_state.recording_duration += (downsampled_samples * 1000) / MIC_SAMPLE_RATE;
                
                // 每秒打印一次状态
                sample_counter += downsampled_samples;
                if (sample_counter >= MIC_SAMPLE_RATE) {
                    ESP_LOGI(TAG, "Recording... duration: %dms, size: %d bytes, volume: %d", 
                            mic_state.recording_duration, mic_state.recording_size, volume);
                    sample_counter = 0;
                }
            }
        }
        
        // 短暂延时
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    free(stereo_buffer);
    free(mono_buffer);
    free(downsampled_buffer);
    if (mic_state.recording_buffer) {
        free(mic_state.recording_buffer);
    }
    vTaskDelete(NULL);
}

/* TTS轮询任务 - 保持不变 */
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
    ESP_LOGI(TAG, "Starting ESP32 Polling Audio Player with PSRAM and Microphone Support...");
    
    // 检查PSRAM是否可用
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM initialized, size: %d bytes", psram_size);
        ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGW(TAG, "PSRAM not found! Large audio files may fail.");
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

    // 初始化I2C
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");

    // 初始化ES8311编解码器
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));

    // 初始化I2S
    ESP_ERROR_CHECK(i2s_init());

    // 创建音频播放任务
    xTaskCreate(audio_playback_task, "audio_playback", 4096, NULL, 10, NULL);

    // 创建麦克风录音任务 - 新增
    xTaskCreate(microphone_recording_task, "mic_recording", 8192, NULL, 9, NULL);

    // 创建TTS轮询任务
    xTaskCreate(tts_polling_task, "tts_polling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready. TTS polling and microphone monitoring started.");
    ESP_LOGI(TAG, "Server URL: %s", TTS_SERVER_URL);
}