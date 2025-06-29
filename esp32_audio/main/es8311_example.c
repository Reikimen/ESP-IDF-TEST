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
#define SAMPLE_RATE         48000
#define SINE_WAVE_FREQ      2000  // 2kHz sine wave
#define AMPLITUDE           8000  // Amplitude of sine wave (16-bit range: -32768 to 32767)
#define DMA_BUF_COUNT       8
#define DMA_BUF_LEN         1024

static const char *TAG = "ES8311_AUDIO";

/* Generate sine wave samples */
static void generate_sine_wave(int16_t *buffer, int num_samples, float frequency, int sample_rate) {
    for (int i = 0; i < num_samples; i++) {
        float sample = AMPLITUDE * sinf(2.0f * M_PI * frequency * i / sample_rate);
        // Stereo: same sample for both channels
        buffer[i * 2] = (int16_t)sample;      // Left channel
        buffer[i * 2 + 1] = (int16_t)sample;  // Right channel
    }
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

/* Initialize ES8311 codec */
static esp_err_t es8311_codec_init(es8311_handle_t *codec_handle) {
    /* Enable codec power */
    gpio_reset_pin(CODEC_ENABLE_PIN);
    gpio_set_direction(CODEC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Enable power amplifier */
    gpio_reset_pin(PA_CTRL_PIN);
    gpio_set_direction(PA_CTRL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_CTRL_PIN, 1);
    
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
    
    /* Configure microphone (analog) */
    es8311_microphone_config(*codec_handle, false);
    
    /* Set output volume (0-100) */
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    
    /* Unmute output */
    es8311_voice_mute(*codec_handle, false);
    
    ESP_LOGI(TAG, "ES8311 initialized successfully");
    return ESP_OK;
}

/* Initialize I2S interface */
static esp_err_t i2s_init(i2s_chan_handle_t *tx_handle) {
    /* I2S configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN;
    
    /* Create I2S channel */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx_handle, NULL));
    
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
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(*tx_handle));
    
    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "ES8311 Audio Example - Playing 2kHz Sine Wave");
    
    /* Initialize I2C */
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");
    
    /* Initialize ES8311 codec */
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));
    
    /* Initialize I2S */
    i2s_chan_handle_t tx_handle;
    ESP_ERROR_CHECK(i2s_init(&tx_handle));
    
    /* Allocate buffer for audio samples */
    size_t buf_size = DMA_BUF_LEN * 2 * sizeof(int16_t);  // Stereo samples
    int16_t *audio_buffer = (int16_t *)malloc(buf_size);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }
    
    /* Generate sine wave */
    generate_sine_wave(audio_buffer, DMA_BUF_LEN, SINE_WAVE_FREQ, SAMPLE_RATE);
    ESP_LOGI(TAG, "Generated %d Hz sine wave", SINE_WAVE_FREQ);
    
    /* Main audio playback loop */
    ESP_LOGI(TAG, "Starting audio playback...");
    size_t bytes_written;
    
    while (1) {
        /* Write audio data to I2S */
        esp_err_t ret = i2s_channel_write(tx_handle, audio_buffer, buf_size, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        }
        
        /* Small delay to prevent watchdog */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    /* Cleanup (never reached in this example) */
    free(audio_buffer);
    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    es8311_delete(codec_handle);
}