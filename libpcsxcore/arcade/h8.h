/* SPDX-License-Identifier: BSD-3-Clause
 * H8/3002 standalone execution interface. ISA derived from MAME, Olivier Galibert.
 * Bus callbacks use logical byte addresses in the H8 big-endian address space.
 */
#ifndef PCSX_ARCADE_H8_H
#define PCSX_ARCADE_H8_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct H8CPU H8CPU;
typedef struct H8Time {
    uint64_t seconds;
    uint64_t attoseconds;
} H8Time;
typedef struct H8SyncTimer {
    H8Time expires;
    uint64_t generation;
    uint8_t armed;
} H8SyncTimer;
typedef struct H8Config {
    void *opaque;
    uint8_t (*read8)(void *opaque, uint32_t address);
    void (*write8)(void *opaque, uint32_t address, uint8_t value);
    /* H8 port numbers: 4, 6, 7, 8, 9, 10 (A), 11 (B). Optional. */
    uint8_t (*port_read)(void *opaque, unsigned port);
    void (*port_write)(void *opaque, unsigned port, uint8_t value, uint8_t ddr);
    void (*serial_tx)(void *opaque, unsigned channel, uint8_t value);
    /* Completed transmitted frames, also with pin wiring installed. Read-only
     * observation for byte-oriented peripherals; does not bypass receive pins. */
    void (*serial_tx_observer)(void *opaque, unsigned channel, uint8_t value);
    /* Optional atomic external word accesses (e.g. C352 execution register). */
    uint16_t (*read16)(void *opaque, uint32_t address);
    void (*write16)(void *opaque, uint32_t address, uint16_t value);
    /* Raw 10-bit analog input, channels 0..7. Unconnected defaults to zero. */
    uint16_t (*adc_read)(void *opaque, unsigned channel);
    /* SCI pins are delivered at each source clock edge. Byte callbacks above
     * are a compatibility fallback when neither pin callback is installed. */
    void (*serial_tx_pin)(void *opaque, unsigned channel, int level);
    void (*serial_clk_pin)(void *opaque, unsigned channel, int level);
    /* Optional fixed external clock period, in integer attoseconds. */
    uint64_t serial_external_period_as[2];
    /* Optional notice when an SCI's source-equivalent global sync timer is
     * adjusted or fired. The scheduler callback itself is intentionally empty. */
    void (*scheduler_timer_changed)(void *opaque, unsigned channel, int armed,
                                    uint64_t generation, uint64_t seconds,
                                    uint64_t attoseconds);
    /* Optional exact idle-loop skip. peek8 returns 1 and the byte at an
     * external address only when reading it has no effect beyond diagnostics
     * and nothing but this CPU can change it during one h8_run_until call
     * (code ROM; shared RAM while the other CPU is stopped). account_reads
     * then receives the byte reads the skip did not perform. */
    int (*peek8)(void *opaque, uint32_t address, uint8_t *value);
    void (*account_reads)(void *opaque, uint32_t address, uint64_t count);
    /* Optional read-only code ROM at external addresses [0, rom_size): reads
     * there use these bytes instead of read8/read16, which must have no read
     * side effects in that range. Writes still go to write8/write16. */
    const uint8_t *rom;
    uint32_t rom_size;
    /* Kart Duel's link board maps ffff10..ffffff as external RAM, overriding
     * the H8 internal peripheral window in the pinned machine map. */
    uint32_t external_io_start;
} H8Config;
typedef struct H8Status {
    uint32_t pc, er[8];
    /* Diagnostic counts since the latest cold/soft reset. */
    uint64_t cycles, instructions;
    /* Instructions the idle-loop skip did not execute, all CPUs since start
     * (performance: executed = instructions - skipped over an interval). */
    uint64_t skipped;
    uint32_t illegal_pc;
    uint16_t illegal_opcode;
    uint8_t ccr, sleeping, halted;
    /* Unsupported peripheral accesses: bit 0 DMA, bit 1 watchdog. */
    uint32_t unsupported_peripherals;
} H8Status;
/* h8_run_until calls, spin skips, instructions run batched and singly. */
void h8_perf_counters(uint64_t out[4]);
H8CPU *h8_create(const H8Config *config);
void h8_destroy(H8CPU *cpu);
/* Savestates: registers, internal RAM/IO, peripherals and clocks as an
 * opaque block of h8_state_size() bytes for this build, taken between
 * h8_run_* calls. A restore keeps the configuration (ROM, callbacks) of the
 * CPU it is applied to. */
size_t h8_state_size(void);
void h8_get_state(const H8CPU *cpu, void *state);
void h8_set_state(H8CPU *cpu, const void *state);
void h8_reset(H8CPU *cpu);
/* Reset at a quiet CPU boundary, never from callbacks. Preserves ER/CCR,
 * internal RAM and mapped GPIO DDR/DR/PCR; resets other modeled peripherals,
 * pending requests and execution/debt state. Sets the absolute clock to the
 * caller's accumulated budget timeline, which may precede prior overshoot.
 * Diagnostic cycles/instructions restart at zero. Does not spend reset-vector
 * cycles until run_cycles. Returns 0, -1 for NULL, -2 during run_cycles.
 * h8_reset remains the original cold reset with clock zero and cleared storage. */
int h8_soft_reset(H8CPU *cpu, uint64_t absolute_cycle);
/* Executes whole instruction states, carries cycle overshoot into the next call. */
void h8_run_cycles(H8CPU *cpu, unsigned cycles);
/* Advances the H8 clock to an absolute cycle deadline. MAME-generated partial
 * states stop after the bus operation that reaches/crosses the deadline; its
 * one-bus-operation overshoot is carried forward and charged after the caller
 * delivers external events at the requested deadline. Returns -1 for NULL or
 * a deadline earlier than the committed clock, -2 if re-entered from a callback. */
int h8_run_until(H8CPU *cpu, uint64_t absolute_cycle);
/* Current emulated H8 cycle, including access/internal costs already charged in
 * the executing state. Callbacks may query this without changing CPU state.
 * Outside state execution (including SCI/ADC tick callbacks and debug accesses)
 * returns committed absolute cycles; cold reset restarts at zero, soft reset
 * uses its caller-supplied epoch (and can discard overshoot). NULL returns zero. */
uint64_t h8_current_cycle(const H8CPU *cpu);
/* IRQ0..IRQ5 are 0..5; NMI is -1. Level is 0/1 (asserted). */
void h8_set_irq(H8CPU *cpu, int line, int level);
/* Diagnostic byte injection; bypasses the physical receive shift register. */
void h8_serial_receive(H8CPU *cpu, unsigned channel, uint8_t value);
/* Physical receive/clock inputs; board devices should use these pin APIs. */
void h8_serial_rx_pin(H8CPU *cpu, unsigned channel, int level);
void h8_serial_clock_pin(H8CPU *cpu, unsigned channel, int level);
/* Deliver a physical pin edge in an explicit scheduler callback-time context.
 * The context is scoped and nested calls restore the prior timestamp. */
void h8_serial_rx_pin_at_time(H8CPU *cpu, unsigned channel, int level,
                              H8Time scheduler_time);
void h8_serial_clock_pin_at_time(H8CPU *cpu, unsigned channel, int level,
                                 H8Time scheduler_time);
/* The global sync timer is distinct from the H8 internal_update deadline.
 * Expiration uses normalized absolute attotime; an unarmed timer reports MAME's
 * `never` timestamp (1,000,000,000 seconds). stop/soft-reset preserve an armed
 * timer; cold reset invalidates it. Query returns 0 on success, -1 for invalid
 * arguments. Peek returns 0 if found, 1 if none are armed, -1 on error.
 * Fire returns 1 for a matching pending generation, 0 for stale/already fired,
 * and -1 for invalid arguments; it never advances SCI or H8 state. The caller
 * checks expiry before firing. Notifications may query state/deliver pins,
 * but must not re-enter execution/reset; the owner arbitrates global timers. */
int h8_get_sync_timer(const H8CPU *cpu, unsigned channel, H8SyncTimer *timer);
int h8_peek_next_sync_timer(const H8CPU *cpu, unsigned *channel,
                            H8SyncTimer *timer);
int h8_fire_sync_timer(H8CPU *cpu, unsigned channel, uint64_t generation);
/* Physical ADTRG pin level; falling edge triggers when ADCR bit7 is set. */
void h8_adc_trigger(H8CPU *cpu, int level);
void h8_get_status(const H8CPU *cpu, H8Status *status);
/* Debug bus accesses include internal RAM/registers and do not spend cycles. */
uint8_t h8_read_byte(H8CPU *cpu, uint32_t address);
void h8_write_byte(H8CPU *cpu, uint32_t address, uint8_t value);
#ifdef __cplusplus
}
#endif
#endif
