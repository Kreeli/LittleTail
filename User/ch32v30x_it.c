/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v30x_it.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2024/03/06
* Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v30x_it.h"
#include "usb_app.h"   /* Audio_SyncTick() */

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  none
 */
void HardFault_Handler(void)
{
  NVIC_SystemReset();
  while (1)
  {
  }
}

void TIM1_UP_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM1_UP_IRQHandler(void)
{
  /*
   * PC7 原来是这里翻转的"呼吸灯"，现在改成**采样率指示**（见 usb_app.c 的
   * Audio_UpdateFsLed()：96k -> 复位/灭，其它 -> 置位/亮）。
   * 所以这里不再碰 PC7，否则会和状态显示打架。
   * TIM1 本身保留（其它用途随时可用），只清标志。
   */
  TIM_ClearITPendingBit(TIM1, TIM_IT_Update);
}

void DMA1_Channel5_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel5_IRQHandler(void){
  if(DMA_GetITStatus(DMA1_IT_HT5) == SET){
    DMA_ClearITPendingBit(DMA1_IT_HT5);
  } 
  else if(DMA_GetITStatus(DMA1_IT_TC5) == SET){
    DMA_ClearITPendingBit(DMA1_IT_TC5);
  }
}

/*
 * TIM2 更新中断：音频同步环的节拍（1kHz，见 Syscfg.c 的 AUDIO_SYNC_TIM_Init）。
 * 反馈值/水位 PI 在这里更新，不依赖主循环的 Delay_Ms。
 * 里面只做整数和 float(硬件 FPU) 运算，不打印、不阻塞。
 */
void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void)
{
  if(TIM_GetITStatus(TIM2, TIM_IT_Update) == SET)
  {
    Audio_SyncTick();
  }
  TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}

