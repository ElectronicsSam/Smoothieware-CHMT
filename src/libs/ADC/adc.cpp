/* mbed Library - ADC
 * Copyright (c) 2010, sblandford
 * released under MIT license http://mbed.org/licence/mit
 */

#include "stm32f407xx.h"
#undef ADC

#include "mbed.h"
#include "adc.h"
#include "mri.h"

#include "pinmap.h"
#include "PeripheralPins.h"

#define STM_ADC ADC1

using namespace mbed;

ADC *ADC::instance;

ADC::ADC(int sample_rate, int cclk_div)
{
    scan_count = 0;
    scan_index = 0;

    memset(scan_chan_lut, 0xFF, sizeof(scan_chan_lut));

    __HAL_RCC_ADC1_CLK_ENABLE();

    // adcclk /8 prescaler
    ADC123_COMMON->CCR |= ADC_CCR_ADCPRE;

    // use long sampling time to reduce isr call freq, to reduce chance of overflow
    // 168 Mhz / 2 (APB CLK) / 8 (ADCCLK) / (480+15) = ~47 us conversion
    // for max 16 scan channels, thats max sampling rate of ~1.3 kHz
    STM_ADC->SMPR1 = ADC_SMPR1_SMP10 | ADC_SMPR1_SMP11 | ADC_SMPR1_SMP12 | ADC_SMPR1_SMP13 | 
                     ADC_SMPR1_SMP14 | ADC_SMPR1_SMP15 | ADC_SMPR1_SMP16 | ADC_SMPR1_SMP17 | 
                     ADC_SMPR1_SMP18;

    STM_ADC->SMPR2 = ADC_SMPR2_SMP0 | ADC_SMPR2_SMP1 | ADC_SMPR2_SMP2 | ADC_SMPR2_SMP3 | 
                     ADC_SMPR2_SMP4 | ADC_SMPR2_SMP5 | ADC_SMPR2_SMP6 | ADC_SMPR2_SMP7 | 
                     ADC_SMPR2_SMP8 | ADC_SMPR2_SMP9;

    // overrun ie, scan mode, end of conv. ie
    STM_ADC->CR1 = ADC_CR1_OVRIE | ADC_CR1_SCAN | ADC_CR1_EOCIE;

    // interrupt after every conversion
    STM_ADC->CR2 = ADC_CR2_EOCS;

    NVIC_SetVector(ADC_IRQn, (uint32_t)&_adcisr);

    _adc_g_isr = NULL;
    instance = this;
};

void ADC::_adcisr(void)
{
    instance->adcisr();
}


void ADC::adcisr(void)
{
    uint16_t data;
    
    // must read data before checking overflow bit
    data = STM_ADC->DR; // to be sure we are valid

    if (STM_ADC->SR & ADC_SR_OVR) {
        // conversion was clobbered by overflow, clear its flag too
        STM_ADC->SR &= ~(ADC_SR_OVR | ADC_SR_EOC);

        // overrun will abort scan sequence, next start will resume from beginning
        scan_index = 0;
        __debugbreak();

    } else if (STM_ADC->SR & ADC_SR_EOC) {
        STM_ADC->SR &= ~ADC_SR_EOC;

        if (_adc_g_isr != NULL && (interrupt_mask & (1 << scan_index)))
            _adc_g_isr(scan_index, data);
        
        if (++scan_index >= scan_count)
            scan_index = 0;
    }
}

uint8_t ADC::_pin_to_channel(PinName pin) {
    uint32_t function = pinmap_find_function(pin, PinMap_ADC);
    uint8_t chan = 0xFF;

    if (function != (uint32_t)NC)
        chan = scan_chan_lut[STM_PIN_CHANNEL(function)];

    return chan;
}

//Enable or disable an ADC pin
void ADC::setup(PinName pin, int state) {
    uint32_t function = pinmap_find_function(pin, PinMap_ADC);
    uint8_t stm_chan = 0xFF;
    uint8_t chan = 0xFF;

    // we don't support dealloc for now, exit early if all channels full or pin doesn't support adc
    if (!state || scan_count >= ADC_CHANNEL_COUNT || function == (uint32_t)NC) 
        return;
    
    stm_chan = STM_PIN_CHANNEL(function);
    chan = scan_count++;

    scan_chan_lut[stm_chan] = chan;

    // configure adc scan channel
    if (chan <= 5) {
        STM_ADC->SQR3 |= (stm_chan << chan);
    } else if (chan <= 11) {
        STM_ADC->SQR2 |= (stm_chan << (chan - 6));
    } else if (chan <= 15) {
        STM_ADC->SQR1 |= (stm_chan << (chan - 12));
    }

    // increase scan count
    STM_ADC->SQR1 = (STM_ADC->SQR1 & (~ADC_SQR1_L)) | (chan << ADC_SQR1_L_Pos);
}

// enable or disable burst mode
void ADC::burst(int state) {
    // turn on adc
    STM_ADC->CR2 |= ADC_CR2_ADON;
}

// set interrupt enable/disable for pin to state
void ADC::interrupt_state(PinName pin, int state) {
    int chan = _pin_to_channel(pin);

    if (chan < ADC_CHANNEL_COUNT) {
        if (state)
            interrupt_mask |= (1 << chan);
        else
            interrupt_mask &= ~(1 << chan);

        // should we set/clear ie bits here too?
        if (interrupt_mask)
            NVIC_EnableIRQ(ADC_IRQn);
        else
            NVIC_DisableIRQ(ADC_IRQn);
    }
}

// append global interrupt handler to function isr
void ADC::append(void(*fptr)(int chan, uint32_t value)) {
    _adc_g_isr = fptr;
}

