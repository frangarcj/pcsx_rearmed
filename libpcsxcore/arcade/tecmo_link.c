/* GPL-2.0-or-later. Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * sony/zn.cpp glpracr2l_state (BSD-3-Clause; see sony_zn_LICENSE.txt).
 * The upstream port map is EMPTY. This runs the real CPU/ROM/RAM while keeping
 * cabinet transport explicitly unimplemented, rather than inventing replies. */
#include "tecmo_link.h"
#include <stdlib.h>
#include <string.h>
struct TecmoLink {
    const uint8_t *rom;
    uint8_t ram[0x8000];
    Z80 *cpu;
    TecmoLinkStatus status;
};
uint8_t tecmo_link_peek(const TecmoLink *s, uint16_t a) {
    return s ? a < 0x8000 ? s->rom[a] : s->ram[a-0x8000] : 0xff;
}
static uint8_t read_memory(void *opaque, uint16_t a) { return tecmo_link_peek(opaque,a); }
static void write_memory(void *opaque, uint16_t a, uint8_t v) {
    TecmoLink *s=opaque; if(a>=0x8000)s->ram[a-0x8000]=v;
}
static uint8_t read_port(void *opaque, uint16_t port) {
    TecmoLink *s=opaque;s->status.io_reads++;s->status.last_read_port=port & 0xff;
    return 0; /* address_map defaults to unmap_value_low; no IRQ source. */
}
static void write_port(void *opaque, uint16_t port, uint8_t data) {
    TecmoLink *s=opaque;s->status.io_writes++;s->status.last_write_port=port & 0xff;
    s->status.last_write_data=data;
}
int tecmo_link_create(const uint8_t *rom,size_t size,TecmoLink **out) {
    if(!rom || size!=0x40000 || !out)return -1;
    *out=NULL;TecmoLink *s=calloc(1,sizeof(*s));if(!s)return -1;s->rom=rom;
    Z80Config cfg={0};cfg.opaque=s;cfg.memory_read=read_memory;cfg.memory_write=write_memory;
    cfg.io_read=read_port;cfg.io_write=write_port;
    s->cpu=z80_create(&cfg);if(!s->cpu){free(s);return -1;}
    *out=s;return 0;
}
void tecmo_link_destroy(TecmoLink *s) { if(s){z80_destroy(s->cpu);free(s);} }
int tecmo_link_sync(TecmoLink *s,uint64_t target) {
    if(!s || target<s->status.cycles)return -1;
    while(target>s->status.cycles && !s->status.faults){
        uint64_t budget=target-s->status.cycles;
        if(budget>UINT32_MAX)budget=UINT32_MAX;
        z80_run_cycles(s->cpu,budget);s->status.cycles+=budget;
        Z80Status st;z80_get_status(s->cpu,&st);if(st.unsupported_operations)s->status.faults|=1;
    }
    return s->status.faults?-1:0;
}
int tecmo_link_reset(TecmoLink *s) {
    if(!s)return -1;
    z80_reset(s->cpu); /* Source RAM and elapsed scheduler time survive reset. */
    return 0;
}
void tecmo_link_status(const TecmoLink *s,TecmoLinkStatus *out) {
    if(!s || !out)return;
    *out=s->status;z80_get_status(s->cpu,&out->cpu);
}
