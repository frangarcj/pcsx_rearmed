/* SPDX-License-Identifier: BSD-3-Clause. MAME adc083x.cpp, smf. */
#ifndef PCSX_ADC0834_H
#define PCSX_ADC0834_H
#include <stdint.h>
typedef struct Adc0834 {
    int cs,clk,di,dout,sars,state,bit,sgl,odd,sel;
    uint8_t channel[4],output;
} Adc0834;
void adc0834_init(Adc0834 *a);
void adc0834_cs(Adc0834 *a,int state);
void adc0834_di(Adc0834 *a,int state);
void adc0834_clk(Adc0834 *a,int state);
#endif
