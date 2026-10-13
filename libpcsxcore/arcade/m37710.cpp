// license:BSD-3-Clause
// copyright-holders:R. Belmont, Karl Stenerud
// Standalone adapter for MAME's M37710 instruction engine. See m37710_LICENSE.txt.
#include "m37710.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <iterator>
#include <new>
#include <vector>
#define ASSERT_LINE 1
#define CLEAR_LINE 0
#define HOLD_LINE 2
#define STATE_GENPCBASE 10000
#define WORD_ALIGNED(a) (((a) & 1) == 0)
#define BIT(v, b) (((v) >> (b)) & 1)
#define LOGMASKED(...) ((void)0)

class m37710_cpu_device {
  public:
#include "m37710_declarations.inc"
    M37710Config cfg;
    uint8_t ram[2048], ports[11], directions[11], io[128], m_int_control[20];
    uint16_t timer_value[8], analog[8];
    uint8_t timer_mode[8], count_start, up_down, adc_control, adc_sweep;
    uint64_t timer_left[8], timer_period[8], timer_ticks[8], adc_left;
    int timer_output[8];
    uint64_t total_cycles, instructions, interrupts, illegal_count, unsupported_count;
    int64_t cycle_debt;
    uint32_t previous_address, instruction_address;
    bool fault;
    std::vector<uint8_t> rom;
    m37710_cpu_device(const M37710Config &c);
    void reset();
    uint64_t run(uint64_t budget);
    uint8_t bus_read8(uint32_t addr);
    uint16_t bus_read16(uint32_t addr);
    void bus_write8(uint32_t addr, uint8_t value);
    void bus_write16(uint32_t addr, uint16_t value);
    bool internal(uint32_t addr) const;
    bool port_address(uint32_t addr, unsigned &port, bool &dir) const;
    int irq_address(uint32_t addr) const;
    void advance(uint64_t cycles);
    uint64_t next_event() const;
    void start_timer(unsigned timer);
    void diagnostic(unsigned kind, uint32_t detail);
    void illegal_opcode();
    void irq_ack(unsigned line);
    void instruction_hook(uint32_t addr) { instruction_address = addr; }
    void logerror(const char *, ...) { diagnostic(M37710_DIAG_PERIPHERAL, m_ir); }
};
struct M37710 : m37710_cpu_device {
    explicit M37710(const M37710Config &c) : m37710_cpu_device(c) {}
};

// clang-format off: instruction macros must precede generated definitions.
#include "m37710_macros.inc"
#include "m37710_inline.inc"
#include "m37710_vectors.inc"
#include "m37710_common.inc"
// clang-format on
#define EXECUTION_MODE EXECUTION_MODE_M0X0
#include "m37710_opcodes.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M0X1
#include "m37710_opcodes.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M1X0
#include "m37710_opcodes.inc"
#undef EXECUTION_MODE
#define EXECUTION_MODE EXECUTION_MODE_M1X1
#include "m37710_opcodes.inc"
#undef EXECUTION_MODE

m37710_cpu_device::m37710_cpu_device(const M37710Config &c) : cfg(c) {
    // Source MAME device_start initializes the CPU register fields to zero.
    m_a = m_b = m_ba = m_bb = m_x = m_y = m_xh = m_yh = m_s = m_pc = m_ppc = m_pg = m_dt = m_dpr =
        0;
    m_flag_e = m_flag_m = m_flag_x = m_flag_n = m_flag_v = m_flag_d = m_flag_i = m_flag_z =
        m_flag_c = 0;
    m_line_irq = m_ipl = m_ir = m_im = m_im2 = m_im3 = m_im4 = m_irq_delay = m_source =
        m_destination = 0;
    m_ICount = 0;
    m_stopped = 0;
    std::memset(ram, 0, sizeof(ram));
    std::memset(ports, 0, sizeof(ports));
    std::memset(directions, 0, sizeof(directions));
    std::memset(io, 0, sizeof(io));
    std::memset(m_int_control, 0, sizeof(m_int_control));
    std::memset(timer_value, 0, sizeof(timer_value));
    std::memset(analog, 0, sizeof(analog));
    std::memset(timer_ticks, 0, sizeof(timer_ticks));
    std::memset(timer_output, 0, sizeof(timer_output));
    adc_control = adc_sweep = 0;
    total_cycles = instructions = interrupts = illegal_count = unsupported_count = 0;
    cycle_debt = 0;
    previous_address = instruction_address = 0;
    fault = false;
    if (c.internal_rom_size)
        rom.assign(c.internal_rom, c.internal_rom + c.internal_rom_size);
    reset();
}
void m37710_cpu_device::reset() {
    std::memset(timer_left, 0, sizeof(timer_left));
    std::memset(timer_period, 0, sizeof(timer_period));
    std::memset(timer_mode, 0, sizeof(timer_mode));
    std::memset(directions, 0, sizeof(directions));
    count_start = up_down = 0;
    adc_left = 0;
    adc_control &= 7;
    adc_sweep = (adc_sweep & ~0xdc) | 3;
    io[0x30] = io[0x38] = 0;
    io[0x34] = (io[0x34] & 0xe0) | 8;
    io[0x3c] = (io[0x3c] & 0xe0) | 8;
    io[0x35] = io[0x3d] = 2;
    io[0x42] &= ~0x1f;
    io[0x5e] = 0;
    io[0x61] &= ~1;
    for (unsigned i = 0; i < 20; i++)
        m_int_control[i] &= i >= 17 ? ~0x3f : ~0xf;
    m_stopped = 0;
    m_ipl = 0;
    m_flag_m = m_flag_x = m_flag_d = 0;
    m_flag_i = 4;
    m_line_irq = m_irq_delay = 0;
    m_dpr = m_pg = m_dt = 0;
    m_s = (m_s & 0xff) | 0x100;
    // Restore the full preserved registers before selecting the reset 16-bit mode.
    m_a |= m_b;
    m_b = 0;
    m_ba |= m_bb;
    m_bb = 0;
    m_x |= m_xh;
    m_xh = 0;
    m_y |= m_yh;
    m_yh = 0;
    m37710i_set_execution_mode(EXECUTION_MODE_M0X0);
    m_pc = bus_read16(0xfffe);
    m_ICount = 0;
    cycle_debt = 0;
    fault = false;
}
void m37710_cpu_device::diagnostic(unsigned kind, uint32_t detail) {
    if (kind == M37710_DIAG_PERIPHERAL)
        unsupported_count++;
    if (cfg.diagnostic)
        cfg.diagnostic(cfg.opaque, kind, m_pg | m_pc, detail);
}
void m37710_cpu_device::illegal_opcode() {
    illegal_count++;
    fault = true;
    m_stopped |= STOP_LEVEL_STOP;
    diagnostic(M37710_DIAG_ILLEGAL, m_ir);
}
void m37710_cpu_device::irq_ack(unsigned line) {
    interrupts++;
    if (cfg.irq_ack)
        cfg.irq_ack(cfg.opaque, line);
}
void m37710_cpu_device::m37710_set_irq_line(int line, int state) {
    if (line < 0 || line >= M37710_LINE_RESET)
        return;
    if (state) {
        m_line_irq |= uint32_t(1) << line;
        if (line < 20)
            m_int_control[line] |= 8;
    } else {
        m_line_irq &= ~(uint32_t(1) << line);
        if (line < 20)
            m_int_control[line] &= ~8;
    }
}
bool m37710_cpu_device::internal(uint32_t a) const {
    if (a < 128) {
        // M37710S4 P0..P3 addresses are external, unlike the M37702 variants.
        return cfg.model != M37710_MODEL_M37710S4 || a < 2 || a >= 10;
    }
    unsigned n = cfg.model == M37710_MODEL_M37710S4 ? 2048 : 512;
    return a < 128 + n || (cfg.model == M37710_MODEL_M37702M2 && a >= 0xc000 && a <= 0xffff);
}
bool m37710_cpu_device::port_address(uint32_t a, unsigned &p, bool &dir) const {
    if (a < 2 || a > 0x15)
        return false;
    // Interleaved port pairs: P0/P1 data at 2/3, direction at 4/5, etc.
    p = ((a - 2) / 4) * 2 + ((a - 2) & 1);
    dir = ((a - 2) & 2) != 0;
    if (p > 8 || (cfg.model == M37710_MODEL_M37710S4 && p < 4))
        return false;
    return true;
}
int m37710_cpu_device::irq_address(uint32_t a) const {
    static const int map[] = {
        M37710_LINE_ADC,       M37710_LINE_UART0XMIT, M37710_LINE_UART0RECV, M37710_LINE_UART1XMIT,
        M37710_LINE_UART1RECV, M37710_LINE_TIMERA0,   M37710_LINE_TIMERA1,   M37710_LINE_TIMERA2,
        M37710_LINE_TIMERA3,   M37710_LINE_TIMERA4,   M37710_LINE_TIMERB0,   M37710_LINE_TIMERB1,
        M37710_LINE_TIMERB2,   M37710_LINE_IRQ0,      M37710_LINE_IRQ1,      M37710_LINE_IRQ2};
    return a >= 0x70 && a <= 0x7f ? map[a - 0x70] : -1;
}
uint8_t m37710_cpu_device::bus_read8(uint32_t a) {
    a &= 0xffffff;
    if (!internal(a))
        return cfg.read8 ? cfg.read8(cfg.opaque, a) : 0xff;
    if (a >= 0xc000)
        return rom[a - 0xc000];
    if (a >= 128)
        return ram[a - 128];
    unsigned p;
    bool dir;
    if (port_address(a, p, dir)) {
        if (dir)
            return directions[p];
        uint8_t d = directions[p],
                v = cfg.port_read ? cfg.port_read(cfg.opaque, p, uint8_t(~d)) : 0;
        return (v & ~d) | (ports[p] & d);
    }
    int irq = irq_address(a);
    if (irq >= 0)
        return m_int_control[irq];
    if (a == 0x1e)
        return adc_control;
    if (a == 0x1f)
        return adc_sweep;
    if (a >= 0x20 && a <= 0x2f)
        return analog[(a - 0x20) / 2] >> ((a & 1) * 8);
    if (a == 0x40)
        return count_start;
    if (a == 0x44)
        return up_down & 31;
    if (a >= 0x46 && a <= 0x55)
        return timer_value[(a - 0x46) / 2] >> ((a & 1) * 8);
    if (a >= 0x56 && a <= 0x5d)
        return timer_mode[a - 0x56];
    if (a == 0x5e)
        return io[a] & 0xf7;
    if (a == 0x30 || a == 0x34 || a == 0x35 || a == 0x38 || a == 0x3c || a == 0x3d || a == 0x61)
        return io[a];
    if (a == 0x36 || a == 0x37 || a == 0x3e || a == 0x3f) {
        diagnostic(M37710_DIAG_PERIPHERAL, a);
        return 0;
    }
    return 0; // Upstream's unused internal range is noprw.
}
uint16_t m37710_cpu_device::bus_read16(uint32_t a) {
    a &= 0xffffff;
    if (!(a & 1) && !internal(a) && !internal((a + 1) & 0xffffff) && cfg.read16)
        return cfg.read16(cfg.opaque, a);
    return bus_read8(a) | (uint16_t(bus_read8(a + 1)) << 8);
}
void m37710_cpu_device::start_timer(unsigned t) {
    timer_left[t] = timer_period[t] = 0;
    if (!(count_start & (1 << t)))
        return;
    if ((timer_mode[t] & 3) != 0) {
        if ((timer_mode[t] & 3) != 1)
            diagnostic(M37710_DIAG_PERIPHERAL, 0x5600 | t);
        return;
    }
    // MAME divides the crystal clocks by 2 for CPU execution; hardware prescalers {2,16,64,512}.
    static const unsigned scales[] = {1, 8, 32, 256};
    timer_period[t] = uint64_t(timer_value[t] + 1) * scales[timer_mode[t] >> 6];
    timer_left[t] = timer_period[t];
}
void m37710_cpu_device::bus_write8(uint32_t a, uint8_t v) {
    a &= 0xffffff;
    if (!internal(a)) {
        if (cfg.write8)
            cfg.write8(cfg.opaque, a, v);
        return;
    }
    if (a >= 0xc000)
        return;
    if (a >= 128) {
        ram[a - 128] = v;
        return;
    }
    unsigned p;
    bool dir;
    if (port_address(a, p, dir)) {
        if (dir)
            directions[p] = v;
        else {
            ports[p] = v;
            if (directions[p] && cfg.port_write)
                cfg.port_write(cfg.opaque, p, v & directions[p], directions[p]);
        }
        return;
    }
    int irq = irq_address(a);
    if (irq >= 0) {
        m_int_control[irq] = v;
        if (irq >= 17 && (v & 0x20))
            diagnostic(M37710_DIAG_PERIPHERAL, a);
        return;
    }
    if (a == 0x1e) {
        if ((v & 64) && !(adc_control & 64)) {
            adc_left = 57 * ((v & 128) ? 2 : 4);
            if (v & 16)
                v &= 0xf8;
        } else if (!(v & 64))
            adc_left = 0;
        adc_control = v;
        return;
    }
    if (a == 0x1f) {
        adc_sweep = v;
        return;
    }
    if (a == 0x40) {
        uint8_t old = count_start;
        count_start = v;
        for (unsigned t = 0; t < 8; t++)
            if ((old ^ v) & (1 << t))
                start_timer(t);
        return;
    }
    if (a == 0x44) {
        up_down = v;
        return;
    }
    if (a >= 0x46 && a <= 0x55) {
        unsigned t = (a - 0x46) / 2, sh = (a & 1) * 8;
        timer_value[t] = (timer_value[t] & ~(255 << sh)) | (v << sh);
        return;
    }
    if (a >= 0x56 && a <= 0x5d) {
        timer_mode[a - 0x56] = v;
        return;
    }
    if (a == 0x34 || a == 0x3c) {
        io[a] = (v & ~8) | (io[a] & 8);
        return;
    }
    if (a == 0x35 || a == 0x3d) {
        io[a] = (io[a] & ((v & 4) ? 0xfa : 0x0a)) | (v & 5);
        return;
    }
    // Match upstream watchdog reload (no side effects) and one-shot start
    // (latched only; one-shot timer execution is still unimplemented in MAME).
    if (a == 0x60) return;
    if (a == 0x30 || a == 0x31 || a == 0x34 || a == 0x35 || a == 0x38 || a == 0x39 || a == 0x3c ||
        a == 0x3d || a == 0x42 || a == 0x5e || a == 0x61) {
        io[a] = v;
        return;
    }
    if (a == 0x32 || a == 0x33 || a == 0x3a || a == 0x3b ||
        (cfg.model == M37710_MODEL_M37710S4 &&
         ((a == 0x1a || a == 0x1c) || a == 0x64 || a == 0x65 || a == 0x62))) {
        io[a] = v;
        diagnostic(M37710_DIAG_PERIPHERAL, a);
    }
}
void m37710_cpu_device::bus_write16(uint32_t a, uint16_t v) {
    a &= 0xffffff;
    if (!(a & 1) && !internal(a) && !internal((a + 1) & 0xffffff) && cfg.write16) {
        cfg.write16(cfg.opaque, a, v);
        return;
    }
    bus_write8(a, v);
    bus_write8(a + 1, v >> 8);
}
uint64_t m37710_cpu_device::next_event() const {
    uint64_t n = adc_left ? adc_left : UINT64_MAX;
    for (unsigned t = 0; t < 8; t++)
        if (timer_left[t])
            n = std::min(n, timer_left[t]);
    return n;
}
void m37710_cpu_device::advance(uint64_t n) {
    total_cycles += n;
    for (unsigned t = 0; t < 8; t++)
        if (timer_left[t]) {
            if (n >= timer_left[t]) {
                uint64_t over = n - timer_left[t];
                timer_ticks[t] += 1 + over / timer_period[t];
                timer_left[t] = timer_period[t] - over % timer_period[t];
                m37710_set_irq_line(M37710_LINE_TIMERA0 - int(t), ASSERT_LINE);
            } else
                timer_left[t] -= n;
        }
    if (adc_left) {
        uint64_t remaining = n;
        while (adc_left && remaining >= adc_left) {
            remaining -= adc_left;
            unsigned channel = adc_control & 7;
            analog[channel] = cfg.analog_read ? cfg.analog_read(cfg.opaque, channel) : 0;
            if (adc_control & 16)
                adc_control = (adc_control & 0xf8) | ((channel + 1) & 7);
            if ((adc_control & 8) || ((adc_control & 16) && channel != (adc_sweep & 3) * 2 + 1))
                adc_left = 57 * ((adc_control & 128) ? 2 : 4);
            else {
                adc_left = 0;
                adc_control &= ~64;
                m37710_set_irq_line(M37710_LINE_ADC, ASSERT_LINE);
            }
        }
        if (adc_left)
            adc_left -= remaining;
    }
}
uint64_t m37710_cpu_device::run(uint64_t budget) {
    if (!budget)
        return 0;
    uint64_t amount = std::min<uint64_t>(budget, INT64_MAX / 2);
    cycle_debt += int64_t(amount);
    uint64_t used = 0;
    while (cycle_debt > 0) {
        m_ICount = 0;
        if (!fault && !(m_stopped & STOP_LEVEL_STOP))
            m37710i_update_irqs();
        if (m_ICount < 0) {
            unsigned n = unsigned(-m_ICount);
            advance(n);
            used += n;
            cycle_debt -= n;
            if (cycle_debt <= 0)
                break;
        }
        if (m_stopped) {
            uint64_t n = uint64_t(cycle_debt);
            if (!fault && !(m_stopped & STOP_LEVEL_STOP))
                n = std::min(n, next_event());
            advance(n);
            used += n;
            cycle_debt -= int64_t(n);
            continue;
        }
        m_ICount = 1;
        previous_address = m_pg | m_pc;
        m_ppc = m_pc;
        m_ir = bus_read8(m_pg | m_pc);
        m_pc++;
        instructions++;
        (this->*m_opcodes[m_ir])();
        // PC carries across program banks, a documented 7700 difference from 65816.
        if (m_pc > 0xffff) {
            m_pg = (m_pg + (m_pc & 0xff0000)) & 0xff0000;
            m_pc &= 0xffff;
        }
        unsigned n = unsigned(std::max(1, 1 - m_ICount));
        advance(n);
        used += n;
        cycle_debt -= n;
    }
    return used;
}
extern "C" {
M37710 *m37710_create(const M37710Config *c) {
    if (!c || c->model > M37710_MODEL_M37710S4)
        return nullptr;
    if (c->model == M37710_MODEL_M37702M2 && (!c->internal_rom || c->internal_rom_size != 16384))
        return nullptr;
    if (c->model != M37710_MODEL_M37702M2 && c->internal_rom_size)
        return nullptr;
    try {
        return new M37710(*c);
    } catch (...) {
        return nullptr;
    }
}
void m37710_destroy(M37710 *c) { delete c; }
void m37710_reset(M37710 *c) {
    if (c)
        c->reset();
}
uint64_t m37710_run_cycles(M37710 *c, uint64_t n) { return c ? c->run(n) : 0; }
void m37710_irq(M37710 *c, unsigned l, int s) {
    if (c)
        c->m37710_set_irq_line(l, s);
}
void m37710_timer_input(M37710 *c, unsigned t, int s) {
    if (!c || t >= 8 || !s || !(c->count_start & (1 << t)))
        return;
    if ((c->timer_mode[t] & 3) != 1) {
        c->diagnostic(M37710_DIAG_PERIPHERAL, 0x100 | t);
        return;
    }
    bool up = t < 5 && ((c->timer_mode[t] & 16) ? c->timer_output[t] : ((c->up_down >> t) & 1));
    c->timer_value[t] += up ? 1 : -1;
}
void m37710_timer_direction(M37710 *c, unsigned t, int u) {
    if (c && t < 8)
        c->timer_output[t] = !!u;
}
void m37710_get_status(const M37710 *c, M37710Status *s) {
    if (!c || !s)
        return;
    std::memset(s, 0, sizeof(*s));
    s->pc = c->m_pg | c->m_pc;
    s->previous_pc = c->previous_address;
    s->pending_irqs = c->m_line_irq;
    s->a = c->m_a | c->m_b;
    s->b = c->m_ba | c->m_bb;
    s->x = c->m_x | c->m_xh;
    s->y = c->m_y | c->m_yh;
    s->s = c->m_s;
    s->dpr = c->m_dpr;
    s->pg = c->m_pg >> 16;
    s->dt = c->m_dt >> 16;
    s->ps = const_cast<M37710 *>(c)->m37710i_get_reg_ps() | (c->m_ipl << 8);
    s->stopped = c->m_stopped;
    s->fault = c->fault;
    s->cycles = c->total_cycles;
    s->instructions = c->instructions;
    s->interrupts = c->interrupts;
    s->illegal_opcodes = c->illegal_count;
    s->unsupported_accesses = c->unsupported_count;
    std::copy(c->timer_ticks, c->timer_ticks + 8, s->timer_ticks);
}
uint32_t m37710_get_register(M37710 *c, unsigned r) {
    if (!c)
        return 0;
    switch (r) {
    case M37710_X:
        return c->m_x | c->m_xh;
    case M37710_Y:
        return c->m_y | c->m_yh;
    case M37710_PS:
        return c->m37710i_get_reg_ps() | (c->m_ipl << 8);
    case M37710_E:
        return 0;
    default:
        return (c->*c->m_get_reg)(r);
    }
}
void m37710_set_register(M37710 *c, unsigned r, uint32_t v) {
    if (!c)
        return;
    switch (r) {
    case M37710_PG:
        c->m_pg = (v & 255) << 16;
        break;
    case M37710_DT:
        c->m_dt = (v & 255) << 16;
        break;
    case M37710_DPR:
        c->m_dpr = v & 65535;
        break;
    case M37710_PS:
        (c->*c->m_set_reg)(r, v);
        c->m_ipl = (v >> 8) & 7;
        break;
    case M37710_X:
        if (c->m_flag_x) {
            c->m_x = v & 255;
            c->m_xh = v & 0xff00;
        } else
            c->m_x = v & 65535;
        break;
    case M37710_Y:
        if (c->m_flag_x) {
            c->m_y = v & 255;
            c->m_yh = v & 0xff00;
        } else
            c->m_y = v & 65535;
        break;
    default:
        (c->*c->m_set_reg)(r, v);
        break;
    }
}
uint8_t m37710_read8(M37710 *c, uint32_t a) { return c ? c->bus_read8(a) : 0xff; }
uint16_t m37710_read16(M37710 *c, uint32_t a) { return c ? c->bus_read16(a) : 0xffff; }
void m37710_write8(M37710 *c, uint32_t a, uint8_t v) {
    if (c)
        c->bus_write8(a, v);
}
void m37710_write16(M37710 *c, uint32_t a, uint16_t v) {
    if (c)
        c->bus_write16(a, v);
}
}
