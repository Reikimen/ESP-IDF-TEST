#include "audio_hal.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es8311.h"

static const char *TAG = "AUDIO_HAL";

/* 全局I2S句柄 */
i2s_chan_handle_t tx_handle = NULL;
i2s_chan_handle_t rx_handle = NULL;

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
    
    es8311_microphone_config(*codec_handle, false);
    es8311_voice_volume_set(*codec_handle, 70, NULL);
    es8311_voice_mute(*codec_handle, false);

    ESP_LOGI(TAG, "ES8311 codec initialized with %dHz sample rate", SAMPLE_RATE);
    return ESP_OK;
}

/* I2S初始化 - 保持不变 */
static esp_err_t i2s_init(void) {
    ESP_LOGI(TAG, "Initializing I2S for playback...");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
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

/* 初始化音频硬件 */
esp_err_t audio_hal_init(void) {
    // 初始化I2C
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized");

    // 初始化ES8311编解码器
    es8311_handle_t codec_handle;
    ESP_ERROR_CHECK(es8311_codec_init(&codec_handle));

    // 初始化I2S
    ESP_ERROR_CHECK(i2s_init());

    return ESP_OK;
}

/* 播放PCM数据 - 保持不变 */
esp_err_t audio_hal_play_pcm(const uint8_t *data, size_t size) {
    if (!tx_handle || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, data, size, &bytes_written, portMAX_DELAY);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (bytes_written != size) {
        ESP_LOGW(TAG, "I2S partial write: %d/%d bytes", bytes_written, size);
    }
    
    return ESP_OK;
}