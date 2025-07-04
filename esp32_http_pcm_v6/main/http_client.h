#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* HTTP Configuration - 保持不变 */
// #define TTS_SERVER_IP          "10.129.113.191"
#define TTS_SERVER_IP          "192.168.32.177"
#define TTS_SERVER_PORT        8001
#define TTS_SERVER_URL         "http://" TTS_SERVER_IP ":8001"
#define DEVICE_ID              "ESP32_VOICE_01"

/* Audio buffer configuration - 使用PSRAM后可以增大缓冲区 */
#define MAX_AUDIO_SIZE         (4 * 1024 * 1024)  // 增大到4MB
#define DOWNLOAD_CHUNK_SIZE    (64 * 1024)        // 增大到64KB
#define POLL_INTERVAL_MS       2000

/* HTTP下载状态 */
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} download_state_t;

/* TTS轮询任务 */
void tts_polling_task(void *pvParameters);

/* 轮询新的TTS内容 */
esp_err_t tts_poll_new_content(char *audio_id, size_t id_size);

/* 下载PCM音频文件 */
esp_err_t download_pcm_audio(const char *audio_id);

#endif /* HTTP_CLIENT_H */