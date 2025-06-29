#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "esp_log.h"
#include "esp_err.h"

/* GPIO Definitions */
#define CODEC_ENABLE_PIN    GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN         GPIO_NUM_40  // Power amplifier control pin
#define I2C_MASTER_SCL_IO   GPIO_NUM_1
#define I2C_MASTER_SDA_IO   GPIO_NUM_2
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  50000
#define ES8311_I2C_ADDR     0x18

/* I2S Configuration */
#define I2S_BCK_PIN         GPIO_NUM_16
#define I2S_WS_PIN          GPIO_NUM_17
#define I2S_DATA_OUT_PIN    GPIO_NUM_18
#define I2S_DATA_IN_PIN     GPIO_NUM_15

/* Audio Configuration */
#define SAMPLE_RATE         48000  // Changed to 48kHz - supported by ES8311 with SCLK as MCLK
#define DMA_BUF_COUNT       8
#define DMA_BUF_LEN         1024
#define RECORD_TIME_SEC     10     // Record for 10 seconds

static const char *TAG = "ES8311_RECORD";

/* Calculate RMS (Root Mean Square) for volume level */
static float calculate_rms(int16_t *buffer, int num_samples) {
    int64_t sum = 0;
    for (int i = 0; i < num_samples; i++) {
        sum += (int64_t)buffer[i] * buffer[i];
    }
    return sqrtf((float)sum / num_samples);
}

/* Convert RMS to dB */
static float rms_to_db(float rms) {
    if (rms <= 0) return -96.0f;  // Minimum dB
    return 20.0f * log10f(rms / 32768.0f);  // 32768 is max value for 16-bit
}

/* Initialize I2C bus */
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

/* Initialize ES8311 codec for recording */
static esp_err_t es8311_codec_init_record(es8311_handle_t *codec_handle) {
    /* Enable codec power */
    gpio_reset_pin(CODEC_ENABLE_PIN);
    gpio_set_direction(CODEC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Power amplifier can be disabled for recording only */
    gpio_reset_pin(PA_CTRL_PIN);
    gpio_set_direction(PA_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTRL_PIN, 0);  // Disable PA for recording
    
    /* Create ES8311 handle */
    *codec_handle = es8311_create(I2C_MASTER_NUM, ES8311_I2C_ADDR);
    if (*codec_handle == NULL) {
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
    
    /* Initialize ES8311 with 16-bit resolution */
    esp_err_t ret = es8311_init(*codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ES8311");
        return ret;
    }
    
    /* Configure microphone (analog) with gain */
    es8311_microphone_config(*codec_handle, false);  // false = analog mic
    
    /* Set microphone gain (0dB to 42dB in 6dB steps) */
    es8311_microphone_gain_set(*codec_handle, ES8311_MIC_GAIN_30DB);
    
    /* Set microphone fade to reduce pop noise */
    es8311_microphone_fade(*codec_handle, ES8311_FADE_64LRCK);
    
    ESP_LOGI(TAG, "ES8311 initialized for recording at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

/* Initialize I2S interface for recording */
static esp_err_t i2s_init_record(i2s_chan_handle_t *rx_handle) {
    /* I2S configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    
    /* Create I2S RX channel */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, rx_handle));
    
    /* Configure I2S standard mode */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // MCLK not connected
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
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(*rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(*rx_handle));
    
    ESP_LOGI(TAG, "I2S RX initialized successfully");
    return ESP_OK;
}

/* Audio processing task */
static void audio_record_task(void *pvParameters) {
    i2s_chan_handle_t rx_handle = (i2s_chan_handle_t)pvParameters;
    
    /* Allocate buffer for audio samples */
    size_t buf_size = DMA_BUF_LEN * 2 * sizeof(int16_t);  // Stereo samples
    int16_t *audio_buffer = (int16_t *)malloc(buf_size);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        vTaskDelete(NULL);
        return;
    }
    
    /* Buffer for mono audio (using left channel only) */
    int16_t *mono_buffer = (int16_t *)malloc(DMA_BUF_LEN * sizeof(int16_t));
    if (mono_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mono buffer");
        free(audio_buffer);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Starting audio recording...");
    ESP_LOGI(TAG, "Recording for %d seconds at %d Hz", RECORD_TIME_SEC, SAMPLE_RATE);
    
    size_t bytes_read;
    int sample_count = 0;
    int total_samples = SAMPLE_RATE * RECORD_TIME_SEC;
    
    /* Optional: Allocate large buffer to store entire recording */
    int16_t *recording_buffer = NULL;
    if (RECORD_TIME_SEC > 0) {
        size_t recording_buffer_size = total_samples * sizeof(int16_t);
        ESP_LOGI(TAG, "Allocating %.2f MB for recording buffer", 
                 recording_buffer_size / (1024.0 * 1024.0));
        
        recording_buffer = (int16_t *)malloc(recording_buffer_size);
        if (recording_buffer == NULL) {
            ESP_LOGW(TAG, "Could not allocate recording buffer, will only show levels");
        }
    }
    
    uint32_t start_time = xTaskGetTickCount();
    
    while (1) {
        /* Read audio data from I2S */
        esp_err_t ret = i2s_channel_read(rx_handle, audio_buffer, buf_size, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            continue;
        }
        
        /* Extract mono audio (left channel) */
        int samples_read = bytes_read / (2 * sizeof(int16_t));  // Stereo samples
        for (int i = 0; i < samples_read; i++) {
            mono_buffer[i] = audio_buffer[i * 2];  // Left channel only
            
            /* Store in recording buffer if allocated */
            if (recording_buffer && sample_count < total_samples) {
                recording_buffer[sample_count++] = mono_buffer[i];
            }
        }
        
        /* Calculate and display audio level */
        float rms = calculate_rms(mono_buffer, samples_read);
        float db = rms_to_db(rms);
        
        /* Create visual level meter */
        int level_bars = (int)((db + 60) / 3);  // Scale -60dB to 0dB into 0-20 bars
        if (level_bars < 0) level_bars = 0;
        if (level_bars > 20) level_bars = 20;
        
        /* Calculate recording progress */
        float progress = (float)sample_count / total_samples * 100.0f;
        
        printf("\r[%3.0f%%] Level: [", progress);
        for (int i = 0; i < 20; i++) {
            if (i < level_bars) {
                printf("=");
            } else {
                printf(" ");
            }
        }
        printf("] %.1f dB", db);
        fflush(stdout);
        
        /* Check if recording time is reached */
        if (RECORD_TIME_SEC > 0 && sample_count >= total_samples) {
            uint32_t end_time = xTaskGetTickCount();
            float actual_duration = (end_time - start_time) / (float)configTICK_RATE_HZ;
            
            printf("\n");
            ESP_LOGI(TAG, "Recording completed!");
            ESP_LOGI(TAG, "Duration: %.2f seconds", actual_duration);
            ESP_LOGI(TAG, "Samples recorded: %d", sample_count);
            
            /* Find max and min values in recording */
            if (recording_buffer) {
                int16_t max_val = -32768, min_val = 32767;
                int64_t sum = 0;
                
                for (int i = 0; i < sample_count; i++) {
                    if (recording_buffer[i] > max_val) max_val = recording_buffer[i];
                    if (recording_buffer[i] < min_val) min_val = recording_buffer[i];
                    sum += recording_buffer[i];
                }
                
                float avg = (float)sum / sample_count;
                ESP_LOGI(TAG, "Audio statistics:");
                ESP_LOGI(TAG, "  Max amplitude: %d", max_val);
                ESP_LOGI(TAG, "  Min amplitude: %d", min_val);
                ESP_LOGI(TAG, "  Average: %.2f", avg);
                
                /* Calculate overall RMS */
                float overall_rms = calculate_rms(recording_buffer, sample_count);
                ESP_LOGI(TAG, "  Overall RMS: %.2f (%.1f dB)", overall_rms, rms_to_db(overall_rms));
                
                /* Here you can save the recording_buffer to SD card or process it */
                ESP_LOGI(TAG, "Recording data is ready for processing/saving");
            }
            
            break;
        }
    }
    
    /* Cleanup */
    free(audio_buffer);
    free(mono_buffer);
    if (recording_buffer) {
        free(recording_buffer);
    }
    
    ESP_LOGI(TAG, "Recording task finished");
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "ES8311 Audio Recording Example");
    
    /* Print memory info */
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    /* Initialize I2C */
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");
    
    /* Initialize ES8311 codec for recording */
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init_record(&codec_handle));
    
    /* Initialize I2S for recording */
    i2s_chan_handle_t rx_handle;
    ESP_ERROR_CHECK(i2s_init_record(&rx_handle));
    
    /* Wait a bit for stabilization */
    vTaskDelay(pdMS_TO_TICKS(100));
    
    /* Create recording task with larger stack size */
    xTaskCreate(audio_record_task, "audio_record", 8192, rx_handle, 5, NULL);
    
    /* Main task can do other things or just wait */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    }
}