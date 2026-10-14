/* GPL-2.0-or-later. MAME sony/zn.cpp glpracr2l_state source map. */
#ifndef PCSX_TECMO_LINK_H
#define PCSX_TECMO_LINK_H
#include <stddef.h>
#include "z80.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TECMO_LINK_CLOCK 4000000u /* Source labels this clock uncertain. */
typedef struct TecmoLink TecmoLink;
typedef struct TecmoLinkStatus {
    uint64_t cycles, io_reads, io_writes;
    uint8_t last_read_port, last_write_port, last_write_data;
    uint32_t faults;
    Z80Status cpu;
} TecmoLinkStatus;
int tecmo_link_create(const uint8_t *rom, size_t size, TecmoLink **out);
void tecmo_link_destroy(TecmoLink *);
int tecmo_link_sync(TecmoLink *, uint64_t cycle);
int tecmo_link_reset(TecmoLink *);
void tecmo_link_status(const TecmoLink *, TecmoLinkStatus *);
/* Debugger read: the source has no main-CPU/shared-memory link mapping. */
uint8_t tecmo_link_peek(const TecmoLink *, uint16_t address);
#ifdef __cplusplus
}
#endif
#endif
