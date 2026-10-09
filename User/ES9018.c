#include "debug.h"
#include "ES9018.h"
#include "stdbool.h"

volatile uint8_t g_es9018_status;
volatile uint8_t g_es9018_status_valid;
volatile uint8_t g_es9018_locked;
volatile uint32_t g_es9018_unlock_count;
volatile uint32_t g_es9018_i2c_errors;
static volatile uint8_t s_lock_poll_due = 1;

#define ES9018_I2C_GUARD 30000u
#define ES9018_I2C_ERRORS 0x0700u /* STAR1: BERR / ARLO / AF */

static uint8_t ES9018_WaitFlag(uint32_t flag, FlagStatus state)
{
    uint32_t guard = ES9018_I2C_GUARD;
    while (I2C_GetFlagStatus(I2C2, flag) != state) {
        if ((I2C2->STAR1 & ES9018_I2C_ERRORS) || (--guard == 0u)) {
            return 0;
        }
    }
    return 1;
}

static void ES9018_ClearAddr(void)
{
    volatile uint16_t dummy;
    dummy = I2C2->STAR1;
    dummy = I2C2->STAR2;
    (void)dummy;
}

static void ES9018_I2CAbort(void)
{
    I2C_GenerateSTOP(I2C2, ENABLE);
    I2C2->STAR1 &= (uint16_t)~ES9018_I2C_ERRORS;
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    g_es9018_i2c_errors++;
}

static uint8_t ES9018_SelectReg(uint8_t reg)
{
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    I2C_NACKPositionConfig(I2C2, I2C_NACKPosition_Current);
    if (!ES9018_WaitFlag(I2C_FLAG_BUSY, RESET)) return 0;
    I2C_GenerateSTART(I2C2, ENABLE);
    if (!ES9018_WaitFlag(I2C_FLAG_SB, SET)) return 0;
    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    if (!ES9018_WaitFlag(I2C_FLAG_ADDR, SET)) return 0;
    ES9018_ClearAddr();
    I2C_SendData(I2C2, reg);
    /* BTF 保证寄存器地址已发完；TXE 只表示发送数据寄存器已空。 */
    return ES9018_WaitFlag(I2C_FLAG_BTF, SET);
}

uint8_t ES9018_TryReadReg(uint8_t reg, uint8_t *value)
{
    uint8_t result;
    uint32_t timer_enabled, usb_enabled;
    if (value == NULL) return 0;
    if (!ES9018_SelectReg(reg)) goto fail;
    I2C_GenerateSTART(I2C2, ENABLE);
    if (!ES9018_WaitFlag(I2C_FLAG_SB, SET)) goto fail;
    /* 单字节接收必须在清 ADDR 之前关闭 ACK。
     * I2C_CheckEvent 会读 STAR2 清 ADDR，不能用它等待接收地址应答。 */
    I2C_AcknowledgeConfig(I2C2, DISABLE);
    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Receiver);
    if (!ES9018_WaitFlag(I2C_FLAG_ADDR, SET)) goto fail;
    timer_enabled = NVIC_GetStatusIRQ(TIM2_IRQn);
    usb_enabled = NVIC_GetStatusIRQ(USBHS_IRQn);
    NVIC_DisableIRQ(TIM2_IRQn);
    NVIC_DisableIRQ(USBHS_IRQn);
    ES9018_ClearAddr();
    I2C_GenerateSTOP(I2C2, ENABLE);
    if (usb_enabled) NVIC_EnableIRQ(USBHS_IRQn);
    if (timer_enabled) NVIC_EnableIRQ(TIM2_IRQn);
    /* 只有清 ADDR / 发 STOP 的几个指令屏蔽中断；等待均允许音频中断。 */
    if (!ES9018_WaitFlag(I2C_FLAG_RXNE, SET)) goto fail;
    result = I2C_ReceiveData(I2C2);
    if (!ES9018_WaitFlag(I2C_FLAG_BUSY, RESET)) goto fail;
    I2C_AcknowledgeConfig(I2C2, ENABLE);
    *value = result;
    return 1;
fail:
    ES9018_I2CAbort();
    return 0;
}

void ES9018_RequestLockPoll(void)
{
    s_lock_poll_due = 1;
}

void ES9018_LockPoll(void)
{
    uint8_t status;
    if (!s_lock_poll_due) return;
    s_lock_poll_due = 0;
    if (!ES9018_TryReadReg(ES9018_REG_CHIP_STATUS, &status) ||
        ((status & ES9018_STATUS_CHIP_ID_MASK) != ES9018_STATUS_CHIP_ID_K2M)) {
        g_es9018_status_valid = 0;
        GPIO_SetBits(GPIOC, GPIO_Pin_7); /* 读取失败不能冒充失锁或锁定 */
        return;
    }
    if (g_es9018_locked && !(status & ES9018_STATUS_LOCK)) {
        g_es9018_unlock_count++;
        GPIO_ResetBits(GPIOC, GPIO_Pin_9); /* 失锁事件锁存，短暂熄灯也不会漏看 */
    }
    g_es9018_status = status;
    g_es9018_status_valid = 1;
    g_es9018_locked = (status & ES9018_STATUS_LOCK) != 0u;
    if (g_es9018_locked) GPIO_ResetBits(GPIOC, GPIO_Pin_7);
    else GPIO_SetBits(GPIOC, GPIO_Pin_7);
}

void ES9018_WriteReg(uint8_t reg, uint8_t value){
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, reg);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_SendData(I2C2, value);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTOP(I2C2, ENABLE);
}

uint8_t ES9018_ReadReg(uint8_t reg){
    uint8_t value;

    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, reg);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Receiver);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED));

    I2C_AcknowledgeConfig(I2C2, DISABLE);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_RXNE) == RESET);

    value = I2C_ReceiveData(I2C2);
    I2C_GenerateSTOP(I2C2, ENABLE);
    return value;
}

void ES9018_SoftwareReset(void)
{
    u8 temp = ES9018_ReadReg(0x00);
    // 软复位特殊处理：写入后芯片立即复位，不能等待ACK
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, 0x00);  // 寄存器地址
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_SendData(I2C2, temp | 0x01);  // 软复位命令
    Delay_Ms(1);               // 等待芯片开始复位

    I2C_GenerateSTOP(I2C2, ENABLE);
    Delay_Ms(1);             // 等待复位完成
}

void ES9018_HardwareReset(void)
{
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);  // 拉低复位
    Delay_Ms(1);
    GPIO_SetBits(GPIOB, GPIO_Pin_14);    // 拉高释放
    Delay_Ms(1);
}

/*
 * 快速复位：只拉一下 RST 再放开，**不重写任何寄存器**
 * （芯片复位后按硬件默认/strap 就能直接工作）。
 *
 * 为什么用 Delay_Us 而不是 Delay_Ms：
 *   1) 总耗时 ~210us，而不是 2ms；
 *   2) Delay_Us 是轮询 SysTick 的循环，**可以被中断打断** ——
 *      USB 中断该来还是来，音频同步包不会被错过。
 * 调用点：Audio_SyncTick()（TIM2 中断）里，见 usb_app.c 的 s_es9018_rst_req。
 */
void ES9018_FastReset(void)
{
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);   /* RST 拉低 */
    Delay_Us(10);                         /* 复位脉冲（最短即可） */
    GPIO_SetBits(GPIOB, GPIO_Pin_14);     /* 释放 */
    Delay_Us(200);                        /* 等芯片内部起来 */
}

void ES9018_Init(void)
{

    ES9018_HardwareReset();
    ES9018_SoftwareReset();
    
    /*
     * Reg0x01：32bit + I2S，固定 I2S 输入（auto=0）。
     * MCU 为 I2S 主机，ES9018 作从机接收。
     */
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, 0xE0); /* 32bit, I2S, 固定 I2S */
    ES9018_WriteReg(0x02, 0x18);
    ES9018_WriteReg(0x03, 0x10);
    ES9018_WriteReg(0x04, 0x00);
    ES9018_WriteReg(0x05, 0x68);
    ES9018_WriteReg(0x06, 0x4A);
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, 0x80 | ES9018_FILTER_SLOW_ROLLOFF); /* 不 mute */
    ES9018_WriteReg(0x08, 0x10);

    /* 保留介入前用户的寄存器配置。 */
    ES9018_WriteReg(0x0A, 0x05);

    ES9018_WriteReg(0x0B, 0x02);
    ES9018_WriteReg(0x0C, 0x5A);
    ES9018_WriteReg(0x0D, 0x00);//使能THD补偿
    /* 失锁时由 DAC 自动静音，保持输出偏置；锁定后自动恢复。 */
    ES9018_WriteReg(0x0E, 0x8A | ES9018_MUTE_ON_LOCK);
    ES9018_WriteReg(0x0F, 0x00); /* 音量 0dB */
    ES9018_WriteReg(0x10, 0x00);
    ES9018_WriteReg(0x11, 0xFF);
    ES9018_WriteReg(0x12, 0xFF);
    ES9018_WriteReg(0x13, 0xFF);
    ES9018_WriteReg(0x14, 0x7F);
    ES9018_WriteReg(0x15, 0x00);

    Delay_Ms(20);
}

void ES9018_SetVolume(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_1, vol);
    ES9018_WriteReg(ES9018_REG_VOLUME_2, vol);
}

void ES9018_SetVolumeL(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_1, vol);
}

void ES9018_SetVolumeR(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_2, vol);
}

void ES9018_Mute(uint8_t mute)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_GENERAL_SETTINGS);
    val &= ~ES9018_MUTE_BOTH;
    val |= (mute & ES9018_MUTE_BOTH);
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, val);
}

void ES9018_SetFilterShape(uint8_t shape)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_GENERAL_SETTINGS);
    val &= ~(3 << 5);
    val |= shape;
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, val);
}

void ES9018_SetInputSelect(uint8_t sel)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_INPUT_CONFIG);
    val &= ~3;
    val |= (sel & 3);
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, val);
}

void ES9018_SetI2SFormat(uint8_t length, uint8_t mode)
{
    /* length/mode 传入已是移位后的宏（如 ES9018_I2S_LENGTH_32BIT） */
    uint8_t val = ES9018_ReadReg(ES9018_REG_INPUT_CONFIG);
    val &= ~((3 << 6) | (3 << 4));
    val |= (length & (3 << 6));
    val |= (mode & (3 << 4));
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, val);
}

uint8_t ES9018_ReadChipStatus(void)
{
    return ES9018_ReadReg(ES9018_REG_CHIP_STATUS);
}

uint8_t ES9018_ReadLockStatus(void)
{
    return (ES9018_ReadReg(ES9018_REG_CHIP_STATUS) & 1);
}

void ES9018_SetBitCLKDIV(uint8_t div){
    u8 val = ES9018_ReadReg(ES9018_REG_MASTER_MODE_CTRL) & 0b10011111;
    switch(div)
    {
        case 4:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_4); 
            break;
        case 8:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_8); 
            break;
        case 16:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_16); 
            break;
        default:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_4); 
            break;
    }
}