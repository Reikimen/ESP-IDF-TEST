#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

// GPIO定义
#define BLINK_GPIO          GPIO_NUM_11       // LED引脚（根据您的板子修改）
#define CODEC_ENABLE_PIN    GPIO_NUM_6         // PREP_VCC_CTL控制ES8311电源
#define PA_CTRL_PIN         GPIO_NUM_40        // 功放控制

// I2C配置
#define I2C_MASTER_SCL_IO   GPIO_NUM_1         // I2C时钟线
#define I2C_MASTER_SDA_IO   GPIO_NUM_2         // I2C数据线
#define I2C_MASTER_NUM      I2C_NUM_0          // I2C端口号
#define I2C_MASTER_FREQ_HZ  100000             // I2C频率100kHz

// I2S配置
#define I2S_BCK_PIN         GPIO_NUM_16        // I2S位时钟
#define I2S_WS_PIN          GPIO_NUM_17        // I2S字选择(LRCK)
#define I2S_DATA_OUT_PIN    GPIO_NUM_18        // I2S数据输出
#define I2S_DATA_IN_PIN     GPIO_NUM_15        // I2S数据输入
#define I2S_PORT_NUM        I2S_NUM_0          // I2S端口号

#define ES8311_ADDR         0x18               // ES8311 I2C地址
#define SAMPLE_RATE         48000              // 采样率

static const char *TAG = "ES8311_I2S";

// I2S配置
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

// 音频缓冲区
#define AUDIO_BUFFER_SIZE   1024
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];

// 初始化GPIO控制引脚
static void init_control_pins(void)
{
    // 配置ES8311电源控制引脚
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CODEC_ENABLE_PIN) | (1ULL << PA_CTRL_PIN) | (1ULL << BLINK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // 使能ES8311电源
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    ESP_LOGI(TAG, "ES8311 power enabled");
    
    // 先关闭功放
    gpio_set_level(PA_CTRL_PIN, 0);
    
    // LED初始状态
    gpio_set_level(BLINK_GPIO, 1);
    
    // 等待电源稳定
    vTaskDelay(pdMS_TO_TICKS(100));
}

// 初始化I2C
static esp_err_t i2c_init(void)
{
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &i2c_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed");
        return ret;
    }
    
    ret = i2c_driver_install(I2C_MASTER_NUM, i2c_config.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed");
        return ret;
    }
    
    ESP_LOGI(TAG, "I2C initialized successfully");
    return ESP_OK;
}

// ES8311 I2C读写函数
static esp_err_t es8311_write_reg(uint8_t reg_addr, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t es8311_read_reg(uint8_t reg_addr, uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ES8311配置（使用内部时钟）
static esp_err_t es8311_codec_init(void)
{
    esp_err_t ret;
    uint8_t chip_id1, chip_id2;
    
    // 读取芯片ID验证通信
    ret = es8311_read_reg(0xFD, &chip_id1);
    ret |= es8311_read_reg(0xFE, &chip_id2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ES8311 chip ID");
        return ret;
    }
    ESP_LOGI(TAG, "ES8311 chip ID: 0x%02X%02X", chip_id1, chip_id2);
    
    // 软复位
    es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(10));
    es8311_write_reg(0x00, 0x00);
    
    // 配置系统时钟（使用内部时钟）
    es8311_write_reg(0x01, 0x30); // 使能内部时钟
    es8311_write_reg(0x02, 0x10); // 时钟分频设置
    es8311_write_reg(0x03, 0x10);
    es8311_write_reg(0x16, 0x24);
    es8311_write_reg(0x04, 0x20);
    es8311_write_reg(0x05, 0x00);
    
    // 配置ADC和DAC
    es8311_write_reg(0x0B, 0x00); // ADC电源
    es8311_write_reg(0x0C, 0x00); // DAC电源
    es8311_write_reg(0x10, 0x03); // ADC数字音量
    es8311_write_reg(0x11, 0x7B);
    es8311_write_reg(0x00, 0x80); // 使能芯片
    
    // 配置I2S接口（从模式，16位）
    es8311_write_reg(0x09, 0x00); // I2S从模式
    es8311_write_reg(0x0A, 0x00); // 16位I2S格式
    
    // 配置ADC
    es8311_write_reg(0x14, 0x1A); // ADC设置
    es8311_write_reg(0x15, 0x53);
    es8311_write_reg(0x1B, 0x00);
    es8311_write_reg(0x1C, 0x6C);
    
    // 配置DAC
    es8311_write_reg(0x37, 0x08);
    es8311_write_reg(0x32, 0xBF); // DAC音量
    
    // 使能DAC和ADC
    es8311_write_reg(0x00, 0xD0); // 使能DAC和ADC电源
    es8311_write_reg(0x12, 0x00); // 取消静音
    
    ESP_LOGI(TAG, "ES8311 initialized successfully");
    
    // 使能功放
    gpio_set_level(PA_CTRL_PIN, 1);
    ESP_LOGI(TAG, "Power amplifier enabled");
    
    return ESP_OK;
}

// 初始化I2S
static esp_err_t i2s_init(void)
{
    // I2S配置
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    // 创建TX和RX通道
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    
    // 标准I2S配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // 不使用MCLK
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
    
    // 初始化TX通道
    if (tx_handle != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    }
    
    // 初始化RX通道
    if (rx_handle != NULL) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    }
    
    ESP_LOGI(TAG, "I2S initialized successfully");
    return ESP_OK;
}

// 生成测试音频（正弦波）
static void generate_sine_wave(int16_t *buffer, int samples, float frequency)
{
    static float phase = 0;
    float phase_increment = 2.0f * M_PI * frequency / SAMPLE_RATE;
    
    for (int i = 0; i < samples; i += 2) {
        int16_t sample = (int16_t)(sinf(phase) * 32767 * 0.5f); // 50%音量
        buffer[i] = sample;     // 左声道
        buffer[i + 1] = sample; // 右声道
        phase += phase_increment;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
    }
}

// 音频回环任务（录音并播放）
static void audio_loopback_task(void *arg)
{
    size_t bytes_written = 0;
    size_t bytes_read = 0;
    
    ESP_LOGI(TAG, "Audio loopback task started");
    
    while (1) {
        // 从麦克风读取音频
        if (rx_handle != NULL) {
            i2s_channel_read(rx_handle, audio_buffer, sizeof(audio_buffer), &bytes_read, portMAX_DELAY);
            
            // 将读取的音频写入扬声器
            if (tx_handle != NULL && bytes_read > 0) {
                i2s_channel_write(tx_handle, audio_buffer, bytes_read, &bytes_written, portMAX_DELAY);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 播放测试音调任务
static void play_test_tone_task(void *arg)
{
    size_t bytes_written = 0;
    
    ESP_LOGI(TAG, "Playing 1kHz test tone");
    
    while (1) {
        // 生成1kHz正弦波
        generate_sine_wave(audio_buffer, AUDIO_BUFFER_SIZE, 1000.0f);
        
        // 播放音频
        if (tx_handle != NULL) {
            i2s_channel_write(tx_handle, audio_buffer, sizeof(audio_buffer), &bytes_written, portMAX_DELAY);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief LED闪烁任务
 */
static void blink_task(void *pvParameter)
{
    // 配置LED引脚
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    
    uint8_t led_state = 0;
    
    while (1) {
        ESP_LOGI(TAG, "LED: %s", led_state ? "ON" : "OFF");
        gpio_set_level(BLINK_GPIO, led_state);
        led_state = !led_state;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ES8311 I2S Audio Example");
    
    // 初始化控制引脚
    init_control_pins();
    
    // 启动LED闪烁任务
    // xTaskCreate(blink_task, "blink_task", 2048, NULL, 5, NULL);
    
    // 初始化I2C
    ESP_ERROR_CHECK(i2c_init());
    
    // 初始化ES8311
    ESP_ERROR_CHECK(es8311_codec_init());
    
    // 初始化I2S
    ESP_ERROR_CHECK(i2s_init());
    
    // 选择运行模式：
    // 1. 音频回环（麦克风到扬声器）
    // xTaskCreate(audio_loopback_task, "audio_loopback", 4096, NULL, 5, NULL);
    
    // 2. 播放测试音调
    xTaskCreate(play_test_tone_task, "play_test_tone", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Audio system initialized and running");
}