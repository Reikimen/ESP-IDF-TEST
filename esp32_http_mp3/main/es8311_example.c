// main/es8311_example.c - 简化版本，不依赖ESP-ADF
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

/* WiFi配置 */
#define WIFI_SSID "CE-Hub-Student"
#define WIFI_PASS "casa-ce-gagarin-public-service"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* TTS服务器配置 - 修改为您的Docker主机IP */
#define TTS_SERVER_HOST    "192.168.1.100"  // 替换为您的实际IP
#define TTS_SERVER_PORT    "8001"
#define TTS_SERVER_URL     "http://" TTS_SERVER_HOST ":" TTS_SERVER_PORT

/* GPIO定义 */
#define CODEC_ENABLE_PIN    GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN         GPIO_NUM_40  // Power amplifier control pin
#define I2C_MASTER_SCL_IO   GPIO_NUM_1
#define I2C_MASTER_SDA_IO   GPIO_NUM_2
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  50000
#define ES8311_I2C_ADDR     0x18

/* I2S配置 */
#define I2S_BCK_PIN         GPIO_NUM_16
#define I2S_WS_PIN          GPIO_NUM_17
#define I2S_DATA_OUT_PIN    GPIO_NUM_18
#define I2S_DATA_IN_PIN     GPIO_NUM_15

/* 音频配置 */
#define SAMPLE_RATE         44100
#define DMA_BUF_COUNT       8
#define DMA_BUF_LEN         1024

/* SPIFFS配置 */
#define SPIFFS_MOUNT_POINT  "/spiffs"
#define MAX_FILE_SIZE       (1024 * 1024 * 2)  // 2MB最大文件大小

static const char *TAG = "ESP32_TTS";
static EventGroupHandle_t s_wifi_event_group;
static i2s_chan_handle_t tx_handle = NULL;
static es8311_handle_t codec_handle = NULL;

/* WiFi事件处理 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi连接断开，正在重连...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
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
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成，连接到: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi连接成功");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "WiFi连接失败");
        return ESP_FAIL;
    }
}

/* 初始化SPIFFS */
static esp_err_t spiffs_init(void) {
    ESP_LOGI(TAG, "初始化SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "挂载或格式化文件系统失败");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "找不到SPIFFS分区");
        } else {
            ESP_LOGE(TAG, "初始化SPIFFS失败 (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取SPIFFS分区信息失败 (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS分区大小: total: %d, used: %d", total, used);
    }

    return ESP_OK;
}

/* HTTP下载回调函数 */
typedef struct {
    FILE *fp;
    size_t total_size;
    size_t downloaded;
} download_context_t;

static esp_err_t http_download_event_handler(esp_http_client_event_t *evt) {
    download_context_t *ctx = (download_context_t *)evt->user_data;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "content-length") == 0) {
                ctx->total_size = atol(evt->header_value);
                ESP_LOGI(TAG, "文件大小: %d 字节", ctx->total_size);
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (ctx->fp) {
                size_t written = fwrite(evt->data, 1, evt->data_len, ctx->fp);
                ctx->downloaded += written;
                if (ctx->total_size > 0) {
                    int progress = (ctx->downloaded * 100) / ctx->total_size;
                    if (progress % 20 == 0) {  // 每20%打印一次进度
                        ESP_LOGI(TAG, "下载进度: %d%%", progress);
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* 下载音频文件 */
static esp_err_t download_audio_file(const char *url, const char *local_path) {
    ESP_LOGI(TAG, "开始下载: %s 到 %s", url, local_path);
    
    FILE *fp = fopen(local_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "无法创建文件: %s", local_path);
        return ESP_FAIL;
    }
    
    download_context_t ctx = {
        .fp = fp,
        .total_size = 0,
        .downloaded = 0
    };
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_download_event_handler,
        .user_data = &ctx,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    fclose(fp);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            ESP_LOGI(TAG, "下载完成，总共下载: %d 字节", ctx.downloaded);
        } else {
            ESP_LOGE(TAG, "HTTP错误码: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* I2C初始化 */
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

/* ES8311编解码器初始化 */
static esp_err_t es8311_codec_init_enhanced(es8311_handle_t *codec_handle) {
    gpio_reset_pin(CODEC_ENABLE_PIN);
    gpio_set_direction(CODEC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    gpio_reset_pin(PA_CTRL_PIN);
    gpio_set_direction(PA_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTRL_PIN, 1);
    
    *codec_handle = es8311_create(I2C_MASTER_NUM, ES8311_I2C_ADDR);
    if (*codec_handle == NULL) {
        ESP_LOGE(TAG, "创建ES8311句柄失败");
        return ESP_FAIL;
    }
    
    es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = false,
        .mclk_frequency = 0,
        .sample_frequency = SAMPLE_RATE,
    };
    
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311初始化失败");
        return ret;
    }
    
    es8311_voice_volume_set(*codec_handle, 80, NULL);
    ESP_LOGI(TAG, "ES8311编解码器初始化成功");
    return ESP_OK;
}

/* I2S初始化 */
static esp_err_t i2s_init_enhanced(i2s_chan_handle_t *tx_handle) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建I2S通道失败");
        return ret;
    }
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DATA_OUT_PIN,
            .din = I2S_DATA_IN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ret = i2s_channel_init_std_mode(*tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S标准模式初始化失败");
        return ret;
    }
    
    ret = i2s_channel_enable(*tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用I2S通道失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "I2S初始化成功");
    return ESP_OK;
}

/* 简化的音频播放函数 - 播放测试音调 */
static esp_err_t play_test_tone(void) {
    ESP_LOGI(TAG, "播放测试音调");
    
    const int tone_freq = 1000;  // 1kHz测试音调
    const int duration_ms = 3000; // 3秒
    const int sample_count = (SAMPLE_RATE * duration_ms) / 1000;
    
    int16_t *audio_buffer = malloc(DMA_BUF_LEN * 2 * sizeof(int16_t));
    if (!audio_buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "开始播放 %dHz 测试音调，持续 %d 秒", tone_freq, duration_ms/1000);
    
    for (int i = 0; i < sample_count; i += DMA_BUF_LEN) {
        int samples_to_generate = (sample_count - i) > DMA_BUF_LEN ? DMA_BUF_LEN : (sample_count - i);
        
        // 生成正弦波测试音调
        for (int j = 0; j < samples_to_generate; j++) {
            float sample = 8000 * sinf(2.0f * M_PI * tone_freq * (i + j) / SAMPLE_RATE);
            audio_buffer[j * 2] = (int16_t)sample;      // Left channel
            audio_buffer[j * 2 + 1] = (int16_t)sample;  // Right channel
        }
        
        size_t bytes_written;
        esp_err_t ret = i2s_channel_write(tx_handle, audio_buffer, 
                                         samples_to_generate * 2 * sizeof(int16_t), 
                                         &bytes_written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S写入失败: %s", esp_err_to_name(ret));
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    free(audio_buffer);
    ESP_LOGI(TAG, "测试音调播放完成");
    return ESP_OK;
}

/* TTS请求和播放任务 */
static void tts_request_and_play_task(void *pvParameters) {
    char *text = (char *)pvParameters;
    char url[512];
    char filename[64];
    char local_path[128];
    
    // 生成唯一文件名
    snprintf(filename, sizeof(filename), "tts_%ld.mp3", time(NULL));
    snprintf(local_path, sizeof(local_path), "%s/%s", SPIFFS_MOUNT_POINT, filename);
    
    // 构建TTS请求URL
    snprintf(url, sizeof(url), "%s/esp32/tts", TTS_SERVER_URL);
    
    ESP_LOGI(TAG, "请求TTS合成: %s", text);
    
    // 发送TTS请求
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    // 构建JSON请求体
    char json_data[1024];
    snprintf(json_data, sizeof(json_data), 
             "{\"text\":\"%s\",\"device_id\":\"esp32_main\"}", text);
    
    esp_http_client_set_post_field(client, json_data, strlen(json_data));
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "TTS请求完成，状态码: %d, 内容长度: %d", status_code, content_length);
        
        if (status_code == 200) {
            // 读取响应
            char response[1024];
            int data_read = esp_http_client_read_response(client, response, sizeof(response)-1);
            response[data_read] = '\0';
            
            ESP_LOGI(TAG, "TTS响应: %s", response);
            
            // 简化的JSON解析 - 提取filename
            char *filename_start = strstr(response, "\"filename\":\"");
            if (filename_start) {
                filename_start += 12;  // 跳过 "filename":"
                char *filename_end = strchr(filename_start, '"');
                if (filename_end) {
                    size_t len = filename_end - filename_start;
                    if (len < sizeof(filename)) {
                        strncpy(filename, filename_start, len);
                        filename[len] = '\0';
                        
                        // 构建下载URL
                        char download_url[512];
                        snprintf(download_url, sizeof(download_url), "%s/esp32/download/%s", TTS_SERVER_URL, filename);
                        
                        ESP_LOGI(TAG, "开始下载音频文件: %s", download_url);
                        
                        // 下载音频文件
                        if (download_audio_file(download_url, local_path) == ESP_OK) {
                            ESP_LOGI(TAG, "音频下载成功！由于暂未实现MP3解码，播放测试音调代替");
                            
                            // 播放测试音调代替MP3播放
                            play_test_tone();
                            
                            // 删除下载的文件
                            if (unlink(local_path) == 0) {
                                ESP_LOGI(TAG, "临时文件已删除: %s", filename);
                            } else {
                                ESP_LOGE(TAG, "删除临时文件失败: %s", local_path);
                            }
                        } else {
                            ESP_LOGE(TAG, "下载音频文件失败");
                        }
                    }
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "TTS请求失败: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(text);
    vTaskDelete(NULL);
}

/* 公共API：请求TTS并播放 */
esp_err_t tts_speak(const char *text) {
    if (!text || strlen(text) == 0) {
        ESP_LOGE(TAG, "文本为空");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查WiFi连接
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi未连接");
        return ESP_ERR_WIFI_NOT_STARTED;
    }
    
    // 复制文本以传递给任务
    char *text_copy = malloc(strlen(text) + 1);
    if (!text_copy) {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    strcpy(text_copy, text);
    
    // 创建TTS请求任务
    xTaskCreate(tts_request_and_play_task, "tts_task", 8192, text_copy, 5, NULL);
    
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "ESP32 TTS音频系统启动 - 简化版本");
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 初始化SPIFFS
    ESP_ERROR_CHECK(spiffs_init());
    
    // 初始化WiFi
    ESP_ERROR_CHECK(wifi_init_sta());
    
    // 初始化I2C
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C初始化完成");
    
    // 初始化ES8311编解码器
    ESP_ERROR_CHECK(es8311_codec_init_enhanced(&codec_handle));
    
    // 初始化I2S
    ESP_ERROR_CHECK(i2s_init_enhanced(&tx_handle));
    
    ESP_LOGI(TAG, "系统初始化完成，可以开始TTS播放");
    
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 播放启动音调
    ESP_LOGI(TAG, "播放系统启动提示音");
    play_test_tone();
    
    // 测试TTS功能
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "开始TTS测试");
    tts_speak("Hello, this is ESP32 TTS system test. System is ready for operation.");
    
    // 主循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "系统运行中... 内存剩余: %d KB", esp_get_free_heap_size() / 1024);
        
        // 定期测试TTS
        static int test_count = 0;
        if (++test_count % 6 == 0) {  // 每60秒测试一次
            char test_message[100];
            snprintf(test_message, sizeof(test_message), "Test message number %d", test_count / 6);
            tts_speak(test_message);
        }
    }
}