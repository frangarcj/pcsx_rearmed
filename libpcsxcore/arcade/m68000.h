/* Standalone MAME-derived MC68000 core adapter. See m68000/LICENSE.txt. */
#ifndef PCSX_ARCADE_M68000_H
#define PCSX_ARCADE_M68000_H
#include "m68000/m68000_compat.h"
#include <cstdint>
#include <functional>
#include <memory>

class M68000 {
public:
    explicit M68000(const M68000Bus &bus);
    /* Do not destroy or move a CPU from one of its bus/IRQ callbacks; those
       callbacks are synchronous and the current run operation owns its state. */
    ~M68000();
    M68000(M68000 &&) noexcept;
    M68000 &operator=(M68000 &&) noexcept;
    M68000(const M68000 &) = delete;
    M68000 &operator=(const M68000 &) = delete;

    void reset();
    /* Advance the MC68000 in clock cycles. Partial instruction state is kept
       between calls; returns cycles consumed, -1 on u64 total-cycle overflow,
       and -2 if called from a synchronous callback. Overflow is checked before
       any CPU or callback-visible state changes. */
    int64_t run(uint32_t cycles);
    void set_irq(unsigned level, bool asserted);
    void set_irq_ack(std::function<void(unsigned)> callback);
    uint32_t pc() const;
    uint16_t sr() const;
    uint32_t data_register(unsigned index) const;
    uint32_t address_register(unsigned index) const;
    bool stopped() const;
    bool bus_error() const;
    bool callback_fault() const;
    uint64_t total_cycles() const;
    /* Source microcode time during bus/IRQ callbacks, completed budget outside
       run(). Partial handlers can issue accesses beyond a short budget; the
       following runs pay that cycle debt. False means u64 time overflow. */
    bool current_cycle(uint64_t &cycle) const;
    /* Scheduler-only: shorten the current run to an absolute CPU deadline.
       Safe in a bus callback, preserves its current timestamp and instruction
       debt. Never extends a slice or accepts a deadline before current time. */
    bool limit_run_to_cycle(uint64_t deadline);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
