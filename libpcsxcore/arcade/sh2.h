/* Standalone SH-2 interpreter, MAME deb2f54f (David Haywood et al.).
 * BSD-3-Clause; see sh2_LICENSE.txt. */
#ifndef PCSX_ARCADE_SH2_H
#define PCSX_ARCADE_SH2_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct StandaloneSh2 Sh2CPU;
typedef struct {
    void *opaque;
    uint32_t (*read)(void *opaque, uint32_t address, unsigned width);
    void (*write)(void *opaque, uint32_t address, unsigned width, uint32_t value);
    void (*irq_ack)(void *opaque, unsigned vector);
} Sh2Config;
typedef struct {
    uint32_t r[16], pc, sr, pr, gbr, vbr, mach, macl, ea, m_delay;
    int32_t icount;
    uint32_t sleep_mode, illegal_pc;
    uint16_t illegal_opcode;
    uint8_t irq_level, irq_vector;
    uint8_t pending_irq[256];
    uint64_t cycles, instructions;
} Sh2State;
Sh2CPU *sh2_create(const Sh2Config *config);
void sh2_destroy(Sh2CPU *cpu);
void sh2_reset(Sh2CPU *cpu, uint64_t absolute_cycle);
int sh2_run_until(Sh2CPU *cpu, uint64_t deadline);
uint64_t sh2_current_cycle(const Sh2CPU *cpu);
void sh2_set_irq(Sh2CPU *cpu, unsigned vector, unsigned level);
void sh2_get_state(const Sh2CPU *cpu, Sh2State *state);
void sh2_set_state(Sh2CPU *cpu, const Sh2State *state);
#ifdef __cplusplus
}
#endif
#endif
