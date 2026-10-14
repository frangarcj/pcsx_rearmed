/* Standalone MAME ADSP2181 interpreter. BSD-3-Clause; see adsp2181_LICENSE.txt. */
#ifndef PCSX_ADSP2181_H
#define PCSX_ADSP2181_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Adsp2181 Adsp2181;
enum { ADSP2181_IRQ0, ADSP2181_IRQ1, ADSP2181_IRQ2, ADSP2181_SPORT0_RX,
 ADSP2181_SPORT0_TX, ADSP2181_TIMER, ADSP2181_IRQE, ADSP2181_BDMA, ADSP2181_IRQL1, ADSP2181_IRQL0 };
#include "adsp2181_registers.inc"
typedef struct Adsp2181Config {
 void *opaque;
 uint32_t (*program_read)(void *,uint16_t word_address);
 void (*program_write)(void *,uint16_t word_address,uint32_t word24);
 uint16_t (*data_read)(void *,uint16_t word_address);
 void (*data_write)(void *,uint16_t word_address,uint16_t word);
 uint16_t (*io_read)(void *,uint16_t word_address);
 void (*io_write)(void *,uint16_t word_address,uint16_t word);
 uint32_t (*sport_rx)(void *,unsigned sport);
 void (*sport_tx)(void *,unsigned sport,uint32_t word);
 void (*dmovlay)(void *,uint32_t overlay);
 void (*timer_enable)(void *,int enabled);
 void (*diagnostic)(void *,uint16_t pc,uint32_t kind);
} Adsp2181Config;
typedef struct Adsp2181Status {
 uint64_t cycles,instructions,interrupts,diagnostic_count;
 uint32_t diagnostics;
 uint16_t pc,previous_pc,imask;
 uint8_t icntl,astat,mstat,sstat,idle,halt;
} Adsp2181Status;
int adsp2181_create(const Adsp2181Config *,Adsp2181 **);
void adsp2181_destroy(Adsp2181 *);
int adsp2181_reset(Adsp2181 *);
int adsp2181_run(Adsp2181 *,uint64_t cycles);
int adsp2181_irq(Adsp2181 *,unsigned line,int state);
int adsp2181_halt(Adsp2181 *,int state);
void adsp2181_status(const Adsp2181 *,Adsp2181Status *);
uint32_t adsp2181_get_register(const Adsp2181 *,unsigned reg);
/* Source SPORT callback may update I registers; core run/reset/IDMA reentry
 * is rejected. Single-owner API, no callbacks may destroy the running CPU. */
int adsp2181_set_register(Adsp2181 *,unsigned reg,uint32_t value);
int adsp2181_idma_address(Adsp2181 *,uint16_t address);
int adsp2181_idma_read(Adsp2181 *,uint16_t *data);
int adsp2181_idma_write(Adsp2181 *,uint16_t data);
#ifdef __cplusplus
}
#endif
#endif
