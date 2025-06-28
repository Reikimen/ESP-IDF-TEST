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
#define CODEC_ENABLE_PIN    GPIO_NUM_6
#define PA_CTRL_PIN         GPIO_NUM_40

// I2C配置
#define I2C_MASTER_SCL_IO   GPIO_NUM_1
#define I2C_MASTER_SDA_IO   GPIO_NUM_2
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  100000  // 降低到100kHz更稳定

// I2S配置
#define I2S_BCK_PIN         GPIO_NUM_16
#define I2S_WS_PIN          GPIO_NUM_17
#define I2S_DATA_OUT_PIN    GPIO_NUM_18
#define I2S_DATA_IN_PIN     GPIO_NUM_15
#define I2S_MCLK_PIN        GPIO_NUM_3   // 如果需要MCLK

#define ES8311_ADDR         0x18

static const char *TAG = "ES8311_DIAG";
static i2s_chan_handle_t tx_handle = NULL;

// ES8311所有寄存器定义
typedef struct {
    uint8_t addr;
    const char *name;
    uint8_t default_val;
} es8311_reg_t;

static const es8311_reg_t es8311_regs[] = {
    {0x00, "RESET", 0x00},
    {0x01, "CLK_MANAGER", 0x30},
    {0x02, "CLK_CTRL", 0x10},
    {0x03, "CLK_CTRL2", 0x10},
    {0x04, "CLK_CTRL3", 0x10},
    {0x05, "CLK_CTRL4", 0x00},
    {0x06, "CLK_CTRL5", 0x00},
    {0x07, "CLK_CTRL6", 0x00},
    {0x08, "CLK_CTRL7", 0x00},
    {0x09, "SDP_IN", 0x00},
    {0x0A, "SDP_OUT", 0x00},
    {0x0B, "SYSTEM", 0x00},
    {0x0C, "SYSTEM2", 0x00},
    {0x0D, "REF", 0x00},
    {0x0E, "REF2", 0x00},
    {0x0F, "GPIO", 0x00},
    {0x10, "ADC_OSR", 0x00},
    {0x11, "ADC_ANA", 0x00},
    {0x12, "ADC_CTRL", 0x00},
    {0x13, "ADC_CTRL2", 0x10},
    {0x14, "ADC_PGA", 0x00},
    {0x15, "ADC_GAIN", 0x00},
    {0x16, "ADC_ALC", 0x00},
    {0x17, "ADC_ALC2", 0x00},
    {0x18, "ADC_ALC3", 0x00},
    {0x19, "ADC_ALC4", 0x00},
    {0x1A, "ADC_ALC5", 0x00},
    {0x1B, "ADC_MUTE", 0x00},
    {0x1C, "ADC_DMIC", 0x00},
    {0x32, "DAC_VOL", 0x00},
    {0x37, "DAC_CTRL", 0x00},
};

// I2C读写函数
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Write reg 0x%02X = 0x%02X", reg, val);
    }
    return ret;
}

static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// 读取所有寄存器
static void dump_all_registers(void)
{
    ESP_LOGI(TAG, "=== ES8311 Register Dump ===");
    uint8_t val;
    
    for (int i = 0; i < sizeof(es8311_regs)/sizeof(es8311_regs[0]); i++) {
        if (es8311_read_reg(es8311_regs[i].addr, &val) == ESP_OK) {
            ESP_LOGI(TAG, "Reg 0x%02X (%s): 0x%02X", 
                     es8311_regs[i].addr, es8311_regs[i].name, val);
        }
    }
}

// ES8311初始化方案1：主模式（ES8311生成时钟）
static esp_err_t init_es8311_master_mode(void)
{
    ESP_LOGI(TAG, "=== Initializing ES8311 in MASTER mode ===");
    
    // 1. 复位
    es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(50));
    es8311_write_reg(0x00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 2. 配置为主模式，内部MCLK
    es8311_write_reg(0x01, 0x3A);  // 主模式，MCLK使能
    
    // 3. 时钟配置 - 对于48kHz
    es8311_write_reg(0x02, 0x00);  // MCLK = 12.288MHz
    es8311_write_reg(0x03, 0x10);  // BCLK分频
    es8311_write_reg(0x04, 0x10);  // LRCK = 48kHz
    es8311_write_reg(0x05, 0x00);
    
    // 4. 系统控制
    es8311_write_reg(0x0B, 0x00);  // 电源参考
    es8311_write_reg(0x0C, 0x00);  // 系统控制
    es8311_write_reg(0x0F, 0x00);  // GPIO设置
    
    // 5. DAC配置
    es8311_write_reg(0x32, 0xBF);  // DAC音量最大
    es8311_write_reg(0x33, 0x00);  // DAC控制
    es8311_write_reg(0x34, 0x00);  // DAC控制
    es8311_write_reg(0x35, 0x00);  // DAC控制
    es8311_write_reg(0x37, 0x08);  // DAC电源和路由
    es8311_write_reg(0x38, 0x00);  // DAC控制
    
    // 6. I2S接口配置 - 主模式，16位
    es8311_write_reg(0x09, 0x04);  // SDIN路由到DAC
    es8311_write_reg(0x0A, 0x50);  // I2S主模式，16位，I2S格式
    
    // 7. 使能DAC
    es8311_write_reg(0x00, 0x80);  // 使能芯片和DAC电源
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 8. 取消静音
    es8311_write_reg(0x12, 0x00);
    es8311_write_reg(0x13, 0x10);
    
    // 9. 配置DAC通路
    es8311_write_reg(0x2D, 0x00);
    es8311_write_reg(0x2E, 0x00);
    es8311_write_reg(0x2F, 0x00);
    es8311_write_reg(0x30, 0x00);
    es8311_write_reg(0x31, 0x00);
    
    ESP_LOGI(TAG, "ES8311 master mode initialization complete");
    return ESP_OK;
}

// ES8311初始化方案2：从模式（ESP32提供时钟）
static esp_err_t init_es8311_slave_mode(void)
{
    ESP_LOGI(TAG, "=== Initializing ES8311 in SLAVE mode ===");
    
    // 1. 复位
    es8311_write_reg(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(50));
    es8311_write_reg(0x00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 2. 配置为从模式
    es8311_write_reg(0x01, 0x30);  // 从模式，使用外部时钟
    
    // 3. 时钟配置
    es8311_write_reg(0x02, 0x10);  // MCLK/LRCK = 256
    es8311_write_reg(0x03, 0x10);  // BCLK = MCLK/8
    es8311_write_reg(0x04, 0x20);  // 时钟分频
    es8311_write_reg(0x05, 0x00);
    es8311_write_reg(0x06, 0x00);
    es8311_write_reg(0x07, 0x00);
    es8311_write_reg(0x08, 0x00);
    
    // 4. 系统和参考电压
    es8311_write_reg(0x0B, 0x00);
    es8311_write_reg(0x0C, 0x00);
    es8311_write_reg(0x0D, 0xFC);  // 参考电压设置
    es8311_write_reg(0x0E, 0x82);
    
    // 5. I2S接口配置 - 从模式，16位
    es8311_write_reg(0x09, 0x04);  // SDIN路由到DAC
    es8311_write_reg(0x0A, 0x00);  // I2S从模式，16位，I2S格式
    
    // 6. DAC配置
    es8311_write_reg(0x32, 0xBF);  // DAC音量最大
    es8311_write_reg(0x33, 0x00);
    es8311_write_reg(0x34, 0x00);
    es8311_write_reg(0x35, 0x00);
    es8311_write_reg(0x36, 0x00);
    es8311_write_reg(0x37, 0x08);  // DAC模块使能
    es8311_write_reg(0x38, 0x00);
    es8311_write_reg(0x39, 0x00);
    
    // 7. 使能DAC电源
    es8311_write_reg(0x00, 0x80);  // 使能芯片
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 8. 额外的DAC设置
    es8311_write_reg(0x2D, 0x12);
    es8311_write_reg(0x2E, 0xC0);
    es8311_write_reg(0x2F, 0x12);
    es8311_write_reg(0x30, 0x16);
    es8311_write_reg(0x31, 0x00);
    
    // 9. 取消静音
    es8311_write_reg(0x12, 0x00);
    es8311_write_reg(0x13, 0x10);
    
    ESP_LOGI(TAG, "ES8311 slave mode initialization complete");
    return ESP_OK;
}

// 初始化I2S（从模式，匹配ES8311主模式）
static esp_err_t init_i2s_slave_mode(void)
{
    ESP_LOGI(TAG, "Initializing I2S in SLAVE mode");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = true;
    
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, 
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DATA_OUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    
    return ESP_OK;
}

// 初始化I2S（主模式）
static esp_err_t init_i2s_master_mode(void)
{
    ESP_LOGI(TAG, "Initializing I2S in MASTER mode");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, 
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,  // 尝试输出MCLK
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DATA_OUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    
    return ESP_OK;
}

// 生成测试音频
static void generate_test_pattern(int16_t *buffer, int samples)
{
    static uint32_t counter = 0;
    
    for (int i = 0; i < samples; i += 2) {
        // 生成1kHz正弦波
        float t = (float)counter / 48000.0f;
        int16_t sample = (int16_t)(sinf(2.0f * M_PI * 1000.0f * t) * 16384);
        
        buffer[i] = sample;      // 左声道
        buffer[i + 1] = sample;  // 右声道
        
        counter++;
        if (counter >= 48000) counter = 0;
    }
}

// 播放测试模式
static void play_test_patterns(void)
{
    int16_t test_buffer[1024];
    size_t bytes_written;
    
    ESP_LOGI(TAG, "=== Playing Test Patterns ===");
    
    // 1. 静音测试
    ESP_LOGI(TAG, "Test 1: Silence (should hear nothing)");
    memset(test_buffer, 0, sizeof(test_buffer));
    for (int i = 0; i < 48; i++) {  // 1秒
        i2s_channel_write(tx_handle, test_buffer, sizeof(test_buffer), 
                         &bytes_written, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 2. DC偏移测试
    ESP_LOGI(TAG, "Test 2: DC offset (should hear click at start/end only)");
    for (int i = 0; i < 1024; i++) {
        test_buffer[i] = 5000;  // 小的DC偏移
    }
    for (int i = 0; i < 48; i++) {  // 1秒
        i2s_channel_write(tx_handle, test_buffer, sizeof(test_buffer), 
                         &bytes_written, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 3. 正弦波测试
    ESP_LOGI(TAG, "Test 3: 1kHz sine wave (should hear clear tone)");
    for (int i = 0; i < 96; i++) {  // 2秒
        generate_test_pattern(test_buffer, 1024);
        i2s_channel_write(tx_handle, test_buffer, sizeof(test_buffer), 
                         &bytes_written, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 4. 方波测试
    ESP_LOGI(TAG, "Test 4: 500Hz square wave (should hear buzzing)");
    int square_period = 48;  // 48000/1000 = 48 samples per period
    for (int j = 0; j < 96; j++) {  // 2秒
        for (int i = 0; i < 1024; i += 2) {
            int16_t sample = ((i/2) % square_period) < (square_period/2) ? 10000 : -10000;
            test_buffer[i] = sample;
            test_buffer[i + 1] = sample;
        }
        i2s_channel_write(tx_handle, test_buffer, sizeof(test_buffer), 
                         &bytes_written, portMAX_DELAY);
    }
}

// 测试不同的配置
static void test_configurations(void)
{
    ESP_LOGI(TAG, "\n=== Testing Configuration 1: ES8311 Master, ESP32 Slave ===");
    
    // 清理之前的I2S
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    
    init_es8311_master_mode();
    vTaskDelay(pdMS_TO_TICKS(100));
    dump_all_registers();
    
    init_i2s_slave_mode();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    play_test_patterns();
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "\n=== Testing Configuration 2: ES8311 Slave, ESP32 Master ===");
    
    // 清理I2S
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    
    init_es8311_slave_mode();
    vTaskDelay(pdMS_TO_TICKS(100));
    dump_all_registers();
    
    init_i2s_master_mode();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    play_test_patterns();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ES8311 Deep Diagnostic ===");
    
    // 1. 初始化GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CODEC_ENABLE_PIN) | (1ULL << PA_CTRL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // 如果有MCLK引脚
    io_conf.pin_bit_mask = (1ULL << I2S_MCLK_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    
    // 2. 关闭功放，使能ES8311
    gpio_set_level(PA_CTRL_PIN, 0);
    gpio_set_level(CODEC_ENABLE_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CODEC_ENABLE_PIN, 1);
    ESP_LOGI(TAG, "ES8311 power enabled");
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 3. 初始化I2C
    i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &i2c_config));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, i2c_config.mode, 0, 0, 0));
    ESP_LOGI(TAG, "I2C initialized");
    
    // 4. 检查ES8311通信
    uint8_t chip_id1, chip_id2;
    if (es8311_read_reg(0xFD, &chip_id1) == ESP_OK && 
        es8311_read_reg(0xFE, &chip_id2) == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 detected! Chip ID: 0x%02X%02X", chip_id1, chip_id2);
    } else {
        ESP_LOGE(TAG, "Failed to detect ES8311!");
        return;
    }
    
    // 5. 读取初始寄存器状态
    ESP_LOGI(TAG, "\n=== Initial Register State ===");
    dump_all_registers();
    
    // 6. 等待一下再开启功放
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(PA_CTRL_PIN, 1);
    ESP_LOGI(TAG, "Power amplifier enabled");
    
    // 7. 测试不同配置
    while (1) {
        test_configurations();
        
        ESP_LOGI(TAG, "\n=== Test cycle complete. Repeating in 5 seconds ===");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}