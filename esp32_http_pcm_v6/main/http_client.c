#include "http_client.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "audio_player.h"

static const char *TAG = "HTTP_CLIENT";

/* 使用PSRAM分配内存的辅助函数 - 保持不变 */
static void* psram_malloc(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        // 如果PSRAM分配失败，尝试使用普通内存
        ESP_LOGW(TAG, "PSRAM allocation failed, trying regular heap");
        ptr = malloc(size);
    }
    return ptr;
}

static void* psram_realloc(void* ptr, size_t size) {
    // 如果原指针为空，直接分配新内存
    if (!ptr) {
        return psram_malloc(size);
    }
    
    // 尝试在PSRAM中重新分配
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (new_ptr) {
        return new_ptr;
    }
    
    // 如果PSRAM重分配失败，尝试普通realloc
    return realloc(ptr, size);
}

/* HTTP下载事件处理器 - 修改为使用PSRAM */
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
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
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

/* 轮询新的TTS内容 - 保持不变 */
esp_err_t tts_poll_new_content(char *audio_id, size_t id_size) {
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
    
    ESP_LOGI(TAG, "Polling for new tasks (Device: %s)...", DEVICE_ID);
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        // 确保头部被完全读取
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        
        ESP_LOGI(TAG, "Poll response: status=%d, content_length=%d, received=%d", 
                 status_code, content_length, poll_state.size);
        
        if (status_code == 200 && poll_state.size > 0) {
            poll_buffer[poll_state.size] = '\0';
            ESP_LOGI(TAG, "Poll response data: %s", poll_buffer);
            
            // 简单JSON解析提取audio_id
            char *audio_id_start = strstr(poll_buffer, "\"audio_id\":\"");
            if (audio_id_start) {
                audio_id_start += 12;
                char *audio_id_end = strchr(audio_id_start, '"');
                if (audio_id_end) {
                    size_t id_len = audio_id_end - audio_id_start;
                    if (id_len < id_size - 1) {
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
        } else if (status_code == 200 && poll_state.size == 0) {
            // 200状态但无内容
            ESP_LOGW(TAG, "Empty response with status 200");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGW(TAG, "Unexpected response: status=%d, size=%d", status_code, poll_state.size);
            if (poll_state.size > 0) {
                poll_buffer[poll_state.size] = '\0';
                ESP_LOGW(TAG, "Response content: %s", poll_buffer);
            }
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP poll failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    return err;
}

/* 下载PCM音频文件 - 修改为使用PSRAM */
esp_err_t download_pcm_audio(const char *audio_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/audio/%s.pcm", TTS_SERVER_URL, audio_id);
    
    ESP_LOGI(TAG, "Downloading PCM: %s", url);
    ESP_LOGI(TAG, "Free heap before download: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // 获取音频状态以释放旧缓冲区
    audio_state_t *state = audio_player_get_state();
    
    // 释放之前的音频缓冲区
    if (state->audio_buffer) {
        free(state->audio_buffer);
        state->audio_buffer = NULL;
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
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        
        if (status_code == 200 && download_state.size > 0) {
            // 成功下载，转移缓冲区所有权给audio_state
            state->audio_buffer = download_state.buffer;
            state->audio_size = download_state.size;
            state->audio_capacity = download_state.capacity;
            state->audio_position = 0;
            state->has_audio = true;
            state->download_complete = true;
            strncpy(state->current_audio_id, audio_id, sizeof(state->current_audio_id) - 1);
            
            ESP_LOGI(TAG, "Downloaded %d bytes for audio: %s", download_state.size, audio_id);
            ESP_LOGI(TAG, "Free heap after download: %d bytes", esp_get_free_heap_size());
            ESP_LOGI(TAG, "Free PSRAM after download: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

/* TTS轮询任务 - 保持不变 */
void tts_polling_task(void *pvParameters) {
    char audio_id[64];
    
    ESP_LOGI(TAG, "TTS polling task started, device ID: %s", DEVICE_ID);
    
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        audio_state_t *state = audio_player_get_state();
        
        // 如果正在播放音频，等待
        if (state->is_playing) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 清空audio_id缓冲区
        memset(audio_id, 0, sizeof(audio_id));
        
        // 轮询新的TTS任务
        esp_err_t err = tts_poll_new_content(audio_id, sizeof(audio_id));
        
        if (err == ESP_OK && strlen(audio_id) > 0) {
            ESP_LOGI(TAG, "🎵 New TTS task: %s", audio_id);
            
            // 下载PCM音频文件
            esp_err_t download_err = download_pcm_audio(audio_id);
            if (download_err == ESP_OK) {
                ESP_LOGI(TAG, "✅ Audio downloaded successfully: %s", audio_id);
                
                // 等待播放完成
                while (state->is_playing || state->has_audio) {
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