#ifndef __HAL_ADC_H__
#define __HAL_ADC_H__
#include "main.h"
#include <stdint.h>

extern volatile uint16_t hal_adc_data[2];

void hal_adc_init(void);

static void hal_adc_rcc_init(void);
static void hal_adc_gpio_init(void);
static void hal_adc_mode_init(void);
static void hal_adc_dma_init(void);
static void hal_adc_dma_arm(void);

uint8_t hal_adc_get_scaled(uint8_t ch);
void hal_adc_process(void);

#endif
