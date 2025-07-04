#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include "esp_err.h"
#include "driver/i2s_std.h"

/* Audio Hardware Configuration - 保持原有引脚设置 */
#define CODEC_ENABLE_PIN       GPIO_NUM_6   // PREP_VCC_CTL - ES8311 power enable
#define PA_CTRL_PIN            GPIO_NUM_40  // Power amplifier control pin

/* I2C Configuration - 保持不变 */
#define I2C_MASTER_NUM         I2C_NUM_0
#define I2C_MASTER_SCL_IO      GPIO_NUM_1  
#define I2C_MASTER_SDA_IO      GPIO_NUM_2   
#define I2C_MASTER_FREQ_HZ     50000
#define ES8311_I2C_ADDR        0x18

/* I2S Configuration - 保持不变 */
#define I2S_NUM                I2S_NUM_0
#define I2S_BCK_IO             GPIO_NUM_16  
#define I2S_WS_IO              GPIO_NUM_17  
#define I2S_DO_IO              GPIO_NUM_18  
#define I2S_DI_IO              GPIO_NUM_15  

/* Audio Configuration */
#define SAMPLE_RATE            48000        // 使用48kHz，与原项目一致
#define BITS_PER_SAMPLE        16
#define DMA_BUF_LEN            1024
#define DMA_BUF_COUNT          8

/* 全局I2S句柄 */
extern i2s_chan_handle_t tx_handle;
extern i2s_chan_handle_t rx_handle;

/* 初始化音频硬件（I2C、I2S、ES8311） */
esp_err_t audio_hal_init(void);

/* 播放PCM数据 */
esp_err_t audio_hal_play_pcm(const uint8_t *data, size_t size);

#endif /* AUDIO_HAL_H */