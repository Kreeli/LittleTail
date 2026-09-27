#pragma once
#include "debug.h"


void USBHS_RCC_init();

const uint8_t *get_dev(uint8_t speed);
const uint8_t *get_cfg(uint8_t speed);
const char *get_str(uint8_t speed, uint8_t index);

void USB_CDC_init(void);
void CDC_WriteBlocking(uint8_t* buf,int size);
void CDC_Notified_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
void CDC_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
void CDC_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

void usb_event_handler(uint8_t busid, uint8_t event);