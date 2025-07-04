#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/* 初始化音频播放器 */
void audio_player_init(void);

/* 获取音频状态 */
audio_state_t* audio_player_get_state(void);

/* 音频播放任务 */
void audio_playback_task(void *pvParameters);

#endif /* AUDIO_PLAYER_H */