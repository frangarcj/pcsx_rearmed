/* GPL-2.0-or-later. Source Primal Rage II VIA/IRQ/DMA/watchdog board wiring. */
#ifndef PCSX_PRIMAL_ZN_BUS_H
#define PCSX_PRIMAL_ZN_BUS_H
#include "vt83c461.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PrimalZnBus PrimalZnBus;
typedef struct PrimalZnBusConfig {
    ata_storage_t *ata; /* borrowed source HDD */
    void *opaque;
    void (*irq10)(void *, int asserted);
} PrimalZnBusConfig;
typedef struct PrimalZnBusStatus {
    uint64_t now_ns, watchdog_deadline, watchdog_kicks, watchdog_resets, dma_reads, dma_read_words, dma_writes;
    uint16_t irq_control, data_latch;
    uint8_t ata_irq, irq10, reset_pending;
    uint32_t faults;
    Vt83c461Status via;
} PrimalZnBusStatus;
int primal_zn_bus_create(const PrimalZnBusConfig *, PrimalZnBus **);
void primal_zn_bus_destroy(PrimalZnBus *);
int primal_zn_bus_sync(PrimalZnBus *, uint64_t now_ns); /* 1 deferred watchdog, -1 fault */
void primal_zn_bus_reset(PrimalZnBus *); /* source latches/config/RAM are preserved */
void primal_zn_bus_ata_irq(PrimalZnBus *, int state);
int primal_zn_bus_read(PrimalZnBus *, uint32_t address, unsigned width, uint32_t *data);
int primal_zn_bus_write(PrimalZnBus *, uint32_t address, unsigned width, uint32_t data);
int primal_zn_bus_dma_read(PrimalZnBus *, uint8_t *ram, size_t ram_bytes, uint32_t address, uint32_t words);
void primal_zn_bus_dma_write(PrimalZnBus *); /* source handler logs only */
void primal_zn_bus_status(const PrimalZnBus *, PrimalZnBusStatus *);
#ifdef __cplusplus
}
#endif
#endif
