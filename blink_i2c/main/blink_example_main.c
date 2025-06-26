/**
 * ESP32-S3 Blink + I2C Scanner
 * 在blink基础上增加I2C扫描功能，查找ES8311设备
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "BLINK_I2C";

// GPIO定义
#define BLINK_GPIO      CONFIG_BLINK_GPIO  // 使用menuconfig配置的LED引脚
#define CODEC_ENABLE    GPIO_NUM_6         // PREP_VCC_CTL控制ES8311电源

// I2C配置
#define I2C_MASTER_SCL_IO   GPIO_NUM_1     // I2C时钟线
#define I2C_MASTER_SDA_IO   GPIO_NUM_2     // I2C数据线
#define I2C_MASTER_NUM      I2C_NUM_0      // I2C端口号
#define I2C_MASTER_FREQ_HZ  100000         // I2C频率100kHz
#define I2C_MASTER_TIMEOUT  1000           // 超时时间(ms)

// ES8311相关定义
#define ES8311_ADDR         0x18           // ES8311的I2C地址
#define ES8311_REG_CHIP_ID1 0xFD           // 芯片ID寄存器1
#define ES8311_REG_CHIP_ID2 0xFE           // 芯片ID寄存器2

/**
 * @brief 初始化I2C主机
 */
static esp_err_t i2c_master_init(void)
{
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
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C master initialized successfully");
    return ESP_OK;
}

/**
 * @brief 扫描I2C总线上的设备
 */
static void i2c_scan_devices(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus...");
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    printf("00:         ");
    
    uint8_t device_count = 0;
    
    for (uint8_t addr = 3; addr < 0x78; addr++) {
        if (addr % 16 == 0) {
            printf("\n%.2x:", addr);
        }
        
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        
        if (err == ESP_OK) {
            printf(" %.2x", addr);
            device_count++;
            
            // 标注已知设备
            if (addr == ES8311_ADDR) {
                ESP_LOGI(TAG, "Found ES8311 at address 0x%02X", addr);
            }
        } else {
            printf(" --");
        }
    }
    
    printf("\n");
    ESP_LOGI(TAG, "Found %d device(s) on I2C bus", device_count);
}

/**
 * @brief 读取ES8311寄存器
 */
static esp_err_t es8311_read_reg(uint8_t reg, uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    
    // 发送寄存器地址
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    
    // 重新开始，读取数据
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT));
    i2c_cmd_link_delete(cmd);
    
    return err;
}

/**
 * @brief 写入ES8311寄存器
 */
static esp_err_t es8311_write_reg(uint8_t reg, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (ES8311_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT));
    i2c_cmd_link_delete(cmd);
    
    return err;
}

/**
 * @brief 测试ES8311通信
 */
static void test_es8311_communication(void)
{
    ESP_LOGI(TAG, "Testing ES8311 communication...");
    
    // 读取芯片ID
    uint8_t id1 = 0, id2 = 0;
    esp_err_t err1 = es8311_read_reg(ES8311_REG_CHIP_ID1, &id1);
    esp_err_t err2 = es8311_read_reg(ES8311_REG_CHIP_ID2, &id2);
    
    if (err1 == ESP_OK && err2 == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 Chip ID: 0x%02X%02X", id1, id2);
        
        // 读取一些基本寄存器
        uint8_t reg_val;
        for (uint8_t reg = 0x00; reg <= 0x02; reg++) {
            if (es8311_read_reg(reg, &reg_val) == ESP_OK) {
                ESP_LOGI(TAG, "Register 0x%02X = 0x%02X", reg, reg_val);
            }
        }
    } else {
        ESP_LOGE(TAG, "Failed to read ES8311 chip ID");
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

/**
 * @brief I2C测试任务
 */
static void i2c_test_task(void *pvParameter)
{
    // 等待系统稳定
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        ESP_LOGI(TAG, "=== I2C Bus Scan ===");
        i2c_scan_devices();
        
        ESP_LOGI(TAG, "=== ES8311 Test ===");
        test_es8311_communication();
        
        // 每10秒扫描一次
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Blink + I2C Scanner Starting...");
    
    // 初始化CODEC电源控制引脚
    gpio_reset_pin(CODEC_ENABLE);
    gpio_set_direction(CODEC_ENABLE, GPIO_MODE_OUTPUT);
    
    // 测试电源控制
    ESP_LOGI(TAG, "Testing CODEC power control...");
    
    // 先关闭电源
    gpio_set_level(CODEC_ENABLE, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "CODEC power OFF");
    
    // 初始化I2C
    ESP_ERROR_CHECK(i2c_master_init());
    
    // 扫描（应该看不到ES8311）
    ESP_LOGI(TAG, "Scan with CODEC power OFF:");
    i2c_scan_devices();
    
    // 开启电源
    gpio_set_level(CODEC_ENABLE, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "CODEC power ON");
    
    // 再次扫描（应该能看到ES8311）
    ESP_LOGI(TAG, "Scan with CODEC power ON:");
    i2c_scan_devices();
    
    // 创建任务
    xTaskCreate(blink_task, "blink_task", 2048, NULL, 5, NULL);
    xTaskCreate(i2c_test_task, "i2c_test_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Tasks created. System running...");
}