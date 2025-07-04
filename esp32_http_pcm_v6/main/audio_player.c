#include "audio_player.h"
#include <string.h>
#include "esp_log.h"
#include "audio_hal.h"

static const char *TAG = "AUDIO_PLAYER";
static audio_state_t audio_state = {0};

/* 初始化音频播放器 */
void audio_player_init(void) {
    memset(&audio_state, 0, sizeof(audio_state));
    ESP_LOGI(TAG, "Audio player initialized");
}

/* 获取音频状态 */
audio_state_t* audio_player_get_state(void) {
    return &audio_state;
}

/* 音频播放任务 - 保持不变 */
void audio_playback_task(void *pvParameters) {
    const size_t chunk_size = 4096;  // 每次写入的数据大小
    
    ESP_LOGI(TAG, "Audio playback task started");
    
    while (1) {
        // 检查是否有音频需要播放
        if (audio_state.has_audio && audio_state.download_complete && !audio_state.is_playing) {
            ESP_LOGI(TAG, "Starting playback of %s (%d bytes)", 
                    audio_state.current_audio_id, audio_state.audio_size);
            
            audio_state.is_playing = true;
            audio_state.audio_position = 0;
            
            // 播放音频数据
            while (audio_state.audio_position < audio_state.audio_size) {
                size_t remaining = audio_state.audio_size - audio_state.audio_position;
                size_t to_write = (remaining > chunk_size) ? chunk_size : remaining;
                
                esp_err_t ret = audio_hal_play_pcm(
                    audio_state.audio_buffer + audio_state.audio_position, 
                    to_write
                );
                
                if (ret == ESP_OK) {
                    audio_state.audio_position += to_write;
                    
                    // 显示播放进度
                    if (audio_state.audio_position % (chunk_size * 10) == 0 || 
                        audio_state.audio_position >= audio_state.audio_size) {
                        int progress = (audio_state.audio_position * 100) / audio_state.audio_size;
                        ESP_LOGD(TAG, "Playback progress: %d%%", progress);
                    }
                } else {
                    ESP_LOGE(TAG, "Audio playback error: %s", esp_err_to_name(ret));
                    break;
                }
                
                // 让出CPU给其他任务
                taskYIELD();
            }
            
            ESP_LOGI(TAG, "Playback completed for %s", audio_state.current_audio_id);
            
            // 重置播放状态
            audio_state.is_playing = false;
            audio_state.has_audio = false;
            audio_state.download_complete = false;
            
            // 清空音频ID以允许重新播放相同的音频
            memset(audio_state.current_audio_id, 0, sizeof(audio_state.current_audio_id));
        }
        
        // 短暂延迟以避免忙等待
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}