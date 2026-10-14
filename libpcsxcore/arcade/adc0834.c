/* SPDX-License-Identifier: BSD-3-Clause. Source MAME machine/adc083x.cpp,
 * deb2f54fe7300e7a192690318beb860159bfd52a, smf. See system573_LICENSE.txt. */
#include "adc0834.h"
#include <string.h>
enum {IDLE,START,MUX,SETTLE,MSB,SE,LSB,FINISHED};
void adc0834_init(Adc0834 *a){memset(a,0,sizeof(*a));a->dout=a->sars=1;}
void adc0834_cs(Adc0834 *a,int s){
    s=!!s;
    if(a->cs!=s){a->state=s?IDLE:START;a->sars=a->dout=1;}a->cs=s;
}
void adc0834_di(Adc0834 *a,int s){a->di=!!s;}
void adc0834_clk(Adc0834 *a,int s){
    s=!!s;
    if(!a->cs && !a->clk && s){
        if(a->state==START && a->di){a->state=MUX;a->sars=0;a->sgl=a->odd=a->sel=a->bit=0;}
        else if(a->state==MUX){
            if(a->bit==0)a->sgl=a->di;else if(a->bit==1)a->odd=a->di;else a->sel=a->di;
            if(++a->bit==3)a->state=SETTLE;
        }else if(a->state==SE){a->sars=0;a->state=LSB;a->bit=1;}
    }
    if(!a->cs && a->clk && !s){
        if(a->state==SETTLE){
            unsigned p=a->odd+a->sel*2;
            double positive=(5*a->channel[p])/255.0;
            double negative=a->sgl?0:(5*a->channel[p^1])/255.0;
            int value=(int)(((positive-negative)*255)/5);
            a->output=value<0?0:value;a->state=MSB;a->bit=7;a->sars=1;a->dout=0;
        }else if(a->state==MSB){a->dout=(a->output>>a->bit)&1;if(--a->bit<0)a->state=SE;}
        else if(a->state==LSB){a->dout=(a->output>>a->bit)&1;if(++a->bit==8)a->state=FINISHED;}
        else if(a->state==FINISHED){a->state=IDLE;a->dout=0;}
    }
    a->clk=s;
}
