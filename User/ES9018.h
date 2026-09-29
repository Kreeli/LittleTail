#ifndef __ES9018_H
#define __ES9018_H

#include "debug.h"

/*****************************************************************************
 * ES9018K2M I2C Address (7-bit)
 *****************************************************************************/
#define ES9018_ADDR        0x90
/*===========================================================================
 * 寄存器地址定义 (p.11)
 *===========================================================================*/
/* 读写寄存器 */
#define ES9018_REG_SYSTEM_SETTINGS        0x00  /* osc_drv, soft_reset */
#define ES9018_REG_INPUT_CONFIG           0x01  /* i2s_length, i2s_mode, input_select */
#define ES9018_REG_RESERVED_02            0x02  /* RESERVED, 默认 0x18, 勿改 */
#define ES9018_REG_RESERVED_03            0x03  /* RESERVED, 默认 0x10, 勿改 */
#define ES9018_REG_AUTOMUTE_TIME          0x04  /* automute_time */
#define ES9018_REG_AUTOMUTE_LEVEL         0x05  /* automute_loopback, automute_level */
#define ES9018_REG_DEEMPH_VOLRATE         0x06  /* deemph, vol_rate */
#define ES9018_REG_GENERAL_SETTINGS       0x07  /* filter_shape, iir_bw, mute */
#define ES9018_REG_GPIO_CONFIG            0x08  /* gpio1_cfg, gpio2_cfg */
#define ES9018_REG_RESERVED_09            0x09  /* RESERVED, 默认 0x00 */
#define ES9018_REG_MASTER_MODE_CTRL       0x0A  /* master_clk_enable, clk_div, sync */
#define ES9018_REG_CHANNEL_MAPPING        0x0B  /* spdif_sel, ch_swap, ch_sel */
#define ES9018_REG_DPLL_ASRC              0x0C  /* dpll_bw_i2s, dpll_bw_dsd */
#define ES9018_REG_THD_COMP               0x0D  /* bypass_thd */
#define ES9018_REG_SOFT_START             0x0E  /* soft_start, mute_on_lock */
#define ES9018_REG_VOLUME_1               0x0F  /* 左声道音量 (0=-0dB, 255=-127.5dB) */
#define ES9018_REG_VOLUME_2               0x10  /* 右声道音量 */
#define ES9018_REG_MASTER_TRIM_0          0x11  /* Master Trim LSB */
#define ES9018_REG_MASTER_TRIM_1          0x12  /* Master Trim */
#define ES9018_REG_MASTER_TRIM_2          0x13  /* Master Trim */
#define ES9018_REG_MASTER_TRIM_3          0x14  /* Master Trim MSB */
#define ES9018_REG_GPIO_INPUT_OSF         0x15  /* gpio_input_sel, bypass_osf/iir */
#define ES9018_REG_THD_COMP_C2_L          0x16  /* 2nd harmonic comp LSB */
#define ES9018_REG_THD_COMP_C2_H          0x17  /* 2nd harmonic comp MSB */
#define ES9018_REG_THD_COMP_C3_L          0x18  /* 3rd harmonic comp LSB */
#define ES9018_REG_THD_COMP_C3_H          0x19  /* 3rd harmonic comp MSB */
#define ES9018_REG_PROG_FILTER_ADDR       0x1A
#define ES9018_REG_PROG_FILTER_COEFF_0    0x1B
#define ES9018_REG_PROG_FILTER_COEFF_1    0x1C
#define ES9018_REG_PROG_FILTER_COEFF_2    0x1D
#define ES9018_REG_PROG_FILTER_CTRL       0x1E

/* 只读寄存器 */
#define ES9018_REG_CHIP_STATUS            0x40  /* chip_id, lock_status, automute_status */
#define ES9018_REG_GPIO_STATUS            0x41
#define ES9018_REG_DPLL_RATIO_0           0x42  /* DPLL ratio LSB */
#define ES9018_REG_DPLL_RATIO_1           0x43
#define ES9018_REG_DPLL_RATIO_2           0x44
#define ES9018_REG_DPLL_RATIO_3           0x45  /* DPLL ratio MSB */

/*===========================================================================
 * SYSTEM SETTINGS (Reg 0x00) 位定义 (p.12)
 *===========================================================================*/
#define ES9018_OSC_DRV_FULL               (0 << 4)
#define ES9018_OSC_DRV_3_4                (8 << 4)
#define ES9018_OSC_DRV_1_2                (12 << 4)
#define ES9018_OSC_DRV_1_4                (14 << 4)
#define ES9018_OSC_DRV_SHUTDOWN           (15 << 4)
#define ES9018_SOFT_RESET                 (1 << 0)
/* 注意: Reg 0 的 bit[3:1] 是 RESERVED, 必须写 0 */

/*===========================================================================
 * INPUT CONFIG (Reg 0x01) 位定义 (p.12)
 *===========================================================================*/
#define ES9018_I2S_LENGTH_16BIT           (0 << 6)
#define ES9018_I2S_LENGTH_24BIT           (1 << 6)
#define ES9018_I2S_LENGTH_32BIT           (2 << 6)  /* 2'd2 或 2'd3 */
#define ES9018_I2S_MODE_I2S               (0 << 4)
#define ES9018_I2S_MODE_LJ                (1 << 4)
#define ES9018_AUTO_INPUT_NORMAL          (0 << 2)
#define ES9018_AUTO_INPUT_I2S_DSD         (1 << 2)
#define ES9018_AUTO_INPUT_I2S_SPDIF       (2 << 2)
#define ES9018_AUTO_INPUT_ALL             (3 << 2)
#define ES9018_INPUT_SEL_I2S              (0)
#define ES9018_INPUT_SEL_SPDIF            (1)
#define ES9018_INPUT_SEL_DSD              (3)

/*===========================================================================
 * GENERAL SETTINGS (Reg 0x07) 位定义 (p.14)
 *===========================================================================*/
#define ES9018_FILTER_FAST_ROLLOFF        (0 << 5)
#define ES9018_FILTER_SLOW_ROLLOFF        (1 << 5)
#define ES9018_FILTER_MIN_PHASE           (2 << 5)
#define ES9018_IIR_BW_NORMAL              (0 << 2)
#define ES9018_IIR_BW_50K                 (1 << 2)
#define ES9018_IIR_BW_60K                 (2 << 2)
#define ES9018_IIR_BW_70K                 (3 << 2)
#define ES9018_MUTE_CH1                   (1 << 0)
#define ES9018_MUTE_CH2                   (1 << 1)
#define ES9018_MUTE_BOTH                  (3 << 0)
/* 注意: Reg 7 bit[7] 和 bit[4] 是 RESERVED, 必须写 0 */

/*===========================================================================
 * MASTER MODE CONTROL (Reg 0x0A) 位定义 (p.16)
 *===========================================================================*/
#define ES9018_MASTER_CLK_EN              (1 << 7)
#define ES9018_CLK_DIV_MCLK_4             (0 << 5)
#define ES9018_CLK_DIV_MCLK_8             (1 << 5)
#define ES9018_CLK_DIV_MCLK_16            (2 << 5)
#define ES9018_SYNC_MODE                  (1 << 4)

/*===========================================================================
 * SOFT START (Reg 0x0E) 位定义 (p.19)
 *===========================================================================*/
#define ES9018_SOFT_START_EN              (1 << 7)
#define ES9018_SOFT_START_ON_LOCK         (1 << 6)
#define ES9018_MUTE_ON_LOCK               (1 << 5)

/*===========================================================================
 * 函数声明
 *===========================================================================*/
void     ES9018_WriteReg(uint8_t reg, uint8_t value);
uint8_t  ES9018_ReadReg(uint8_t reg);

void     ES9018_Init(void);
void     ES9018_SoftwareReset(void);
void ES9018_HardwareReset(void);

void     ES9018_SetVolume(uint8_t vol);       /* 左右声道设为相同值 */
void     ES9018_SetVolumeL(uint8_t vol);      /* 左声道 0=-0dB, 255=-127.5dB */
void     ES9018_SetVolumeR(uint8_t vol);      /* 右声道 */
void     ES9018_Mute(uint8_t mute);           /* bit0=左, bit1=右 */
void     ES9018_SetFilterShape(uint8_t shape);
void     ES9018_SetInputSelect(uint8_t sel);
void     ES9018_SetI2SFormat(uint8_t length, uint8_t mode);

uint8_t  ES9018_ReadChipStatus(void);
uint8_t  ES9018_ReadLockStatus(void);
void ES9018_SetBitCLKDIV(uint8_t div);
#endif /* __ES9018_H */
