/* Standalone driver around the pinned MAME MC68000 microcoded core. */
#include "m68000.h"
#include "m68000/m68000_internal.h"
#include <algorithm>
#include <cstring>

M68kBusAccess::M68kBusAccess(m68000_device *owner, M68kSpace space)
    : owner_(owner), space_(space)
{
}

uint16_t M68kBusAccess::read_interruptible(uint32_t address, uint16_t mem_mask)
{
    return owner_->bus_read(space_, address, mem_mask);
}

void M68kBusAccess::write_interruptible(uint32_t address, uint16_t data,
                                        uint16_t mem_mask)
{
    owner_->bus_write(space_, address, data, mem_mask);
}

m68000_device::m68000_device(const M68000Bus &bus)
    : m_program(this, M68kSpace::Program),
      m_opcodes(this, M68kSpace::Opcode),
      m_cpu_space(this, M68kSpace::Cpu),
      m_bus(bus)
{
    m_total_cycles = bus.cycle_origin;
    m_handlers_f = s_handlers_df;
    m_handlers_p = s_handlers_dp;
    init_decode_table();
    reset();
}

void m68000_device::abort_access(u32 reason)
{
    m_post_run = reason;
    m_post_run_cycles = m_icount;
    m_icount = 0;
}

void m68000_device::do_post_run()
{
    m_icount = m_post_run_cycles;
    m_post_run_cycles = 0;
    switch (m_post_run) {
    case PR_BERR:
        m_inst_state = S_BUS_ERROR;
        m_inst_substate = 0;
        m_icount -= 10;
        break;
    default:
        break;
    }
    m_post_run = 0;
}

void m68000_device::execute_run(int cycles)
{
    if (cycles <= 0)
        return;
    m_slice_cycles = cycles;
    m_running = true;
    m_icount = cycles;
    m_icount -= m_count_before_instruction_step;
    if (m_icount < 0) {
        m_count_before_instruction_step = -m_icount;
        m_icount = 0;
    } else {
        m_count_before_instruction_step = 0;
    }

    for (;;) {
        if (m_icount > 0 && m_inst_substate)
            (this->*(m_handlers_p[m_inst_state]))();
        while (m_icount > 0) {
            if (m_inst_state >= S_first_instruction) {
                m_ipc = m_pc - 2;
                m_irdi = m_ird;
            }
            (this->*(m_handlers_f[m_inst_state]))();
        }
        if (m_post_run)
            do_post_run();
        else
            break;
    }
    if (m_icount < 0) {
        m_count_before_instruction_step = -m_icount;
        m_icount = 0;
    }
    m_total_cycles += static_cast<u64>(m_slice_cycles);
    m_running = false;
}

bool m68000_device::current_cycle(u64 &cycle) const
{
    const int remaining = m_post_run ? m_post_run_cycles : m_icount;
    const u64 elapsed = m_running ? static_cast<u64>(static_cast<int64_t>(m_slice_cycles) - remaining) : 0;
    if (elapsed > UINT64_MAX - m_total_cycles) return false;
    cycle = m_total_cycles + elapsed;
    return true;
}

bool m68000_device::limit_run_to_cycle(u64 deadline)
{
    u64 current;
    if (!m_running || !current_cycle(current) || deadline < current) return false;
    if (deadline - m_total_cycles >= u64(m_slice_cycles)) return true;
    const int budget = int(deadline - m_total_cycles);
    const int drop = m_slice_cycles - budget;
    if (m_post_run) m_post_run_cycles -= drop;
    else m_icount -= drop;
    m_slice_cycles = budget;
    return true;
}

void m68000_device::reset()
{
    m_inst_state = S_RESET;
    m_inst_substate = 0;
    m_count_before_instruction_step = 0;
    m_post_run = 0;
    m_post_run_cycles = 0;
    m_last_vpa_time = 0;
    m_bus_error = false;
    update_user_super();
}

void m68000_device::update_user_super()
{
    m_sp = (m_sr & SR_S) ? 16 : 15;
}

void m68000_device::execute_set_input(int inputnum, int state)
{
    if (inputnum < 1 || inputnum > 7)
        return;
    const u32 old_level = m_int_level;
    if (state)
        m_virq_state |= 1u << inputnum;
    else
        m_virq_state &= ~(1u << inputnum);
    u32 level = 0;
    for (u32 i = 7; i > 0; --i) {
        if (m_virq_state & (1u << i)) {
            level = i;
            break;
        }
    }
    m_int_level = level;
    if (old_level != 7 && level == 7)
        m_nmi_pending = 1;
    update_interrupt();
}

void m68000_device::update_interrupt()
{
    if (m_nmi_pending)
        m_int_next_state = (7u << 24) | S_INTERRUPT;
    else if (m_int_level > ((m_sr >> 8) & 7))
        m_int_next_state = (m_int_level << 24) | S_INTERRUPT;
    else
        m_int_next_state = 0;
}

void m68000_device::init_decode_table()
{
    m_decode_table.assign(0x10000, S_ILLEGAL);
    for (int i = 0; s_packed_decode_table[i].mask; ++i) {
        const u16 value = s_packed_decode_table[i].value;
        const u16 mask = s_packed_decode_table[i].mask;
        const u16 state = s_packed_decode_table[i].state;
        u16 cvalue = 0;
        do {
            if (m_decode_table[value | cvalue] == S_ILLEGAL)
                m_decode_table[value | cvalue] = state;
            cvalue = ((cvalue | mask) + 1) & ~mask;
        } while (cvalue);
    }
}

void m68000_device::set_ftu_const()
{
    switch (m_ird >> 12) {
    case 0x4: m_ftu = 0x80; break;
    case 0x5: case 0xe: m_ftu = (m_ird >> 9) & 7; if (!m_ftu) m_ftu = 8; break;
    case 0x6: case 0x7: m_ftu = s8(m_ird); break;
    case 0x8: case 0xc: m_ftu = 0xf; break;
    default: m_ftu = 0; break;
    }
}

void m68000_device::start_interrupt_vector_lookup()
{
    const int level = static_cast<int>(m_next_state >> 24);
    if (m_irq_ack) {
        ++m_callback_depth;
        m_irq_ack(level);
        --m_callback_depth;
    }
    if (level == 7) {
        m_nmi_pending = 0;
        update_interrupt();
    }
}

void m68000_device::end_interrupt_vector_lookup()
{
    m_int_vector = (m_edb & 0xff) << 2;
    m_int_next_state = 0;
}

u16 m68000_device::get_fc() const noexcept
{
    return (m_base_ssw | (m_sr & SR_S ? SSW_S : 0)) & 7;
}

u16 m68000_device::bus_read(M68kSpace space, u32 address, u16 mem_mask)
{
    u16 value = m_edb;
    address &= 0x00ffffff;
    if (space == M68kSpace::Cpu && m_bus.use_default_autovectors &&
        address >= 0x00fffff0) {
        /* MAME's default CPU-space map returns autovector bytes at odd lanes. */
        const u32 mapped = address | 1;
        if (mapped <= 0x00ffffff && (mapped & 0x0f) >= 3 && (mapped & 0x0f) <= 15 && (mapped & 1)) {
            const u16 vector = static_cast<u16>(0x18 + ((mapped & 0x0f) - 1) / 2);
            return static_cast<u16>((value & 0xff00) | vector);
        }
    }
    bool ok = false;
    if (m_bus.read_word) {
        ++m_callback_depth;
        ok = m_bus.read_word(m_bus.opaque, space, address, mem_mask, &value);
        --m_callback_depth;
    }
    if (!ok) {
        m_bus_error = true;
        abort_access(PR_BERR);
    }
    return value;
}

void m68000_device::bus_write(M68kSpace space, u32 address, u16 data,
                              u16 mem_mask)
{
    address &= 0x00ffffff;
    bool ok = false;
    if (m_bus.write_word) {
        ++m_callback_depth;
        ok = m_bus.write_word(m_bus.opaque, space, address, data, mem_mask);
        --m_callback_depth;
    }
    if (!ok) {
        m_bus_error = true;
        abort_access(PR_BERR);
    }
}

class M68000::Impl : public m68000_device {
public:
    explicit Impl(const M68000Bus &bus) : m68000_device(bus) {}
};

M68000::M68000(const M68000Bus &bus) : impl_(std::make_unique<Impl>(bus)) {}
M68000::~M68000() = default;
M68000::M68000(M68000 &&) noexcept = default;
M68000 &M68000::operator=(M68000 &&) noexcept = default;
void M68000::reset()
{
    if (impl_->callback_active()) {
        impl_->note_callback_fault();
        return;
    }
    impl_->reset();
}
int64_t M68000::run(uint32_t cycles)
{
    if (impl_->callback_active()) {
        impl_->note_callback_fault();
        return -2;
    }
    if (cycles > UINT64_MAX - impl_->total_cycles())
        return -1;
    const uint32_t requested = cycles;
    while (cycles) {
        const uint32_t slice = std::min<uint32_t>(cycles, 0x7fffffff);
        const uint64_t before = impl_->total_cycles();
        impl_->execute_run(static_cast<int>(slice));
        const uint64_t actual = impl_->total_cycles() - before;
        if (actual < slice) return requested - cycles + actual;
        cycles -= slice;
    }
    return requested;
}
void M68000::set_irq(unsigned level, bool asserted)
{
    if (level >= 1 && level <= 7)
        impl_->execute_set_input(static_cast<int>(level), asserted ? 1 : 0);
}
void M68000::set_irq_ack(std::function<void(unsigned)> callback)
{
    if (impl_->callback_active()) {
        impl_->note_callback_fault();
        return;
    }
    impl_->set_irq_ack([callback = std::move(callback)](int level) {
        if (callback) callback(static_cast<unsigned>(level));
    });
}
uint32_t M68000::pc() const { return impl_->pc(); }
uint16_t M68000::sr() const { return impl_->sr(); }
uint32_t M68000::data_register(unsigned index) const { return index < 8 ? impl_->reg(index) : 0; }
uint32_t M68000::address_register(unsigned index) const
{
    return index < 7 ? impl_->reg(index + 8) : index == 7 ? impl_->active_sp() : 0;
}
bool M68000::stopped() const { return impl_->stopped(); }
bool M68000::bus_error() const { return impl_->bus_error(); }
bool M68000::callback_fault() const { return impl_->callback_fault(); }
uint64_t M68000::total_cycles() const { return impl_->total_cycles(); }
bool M68000::current_cycle(uint64_t &cycle) const { return impl_->current_cycle(cycle); }
bool M68000::limit_run_to_cycle(uint64_t deadline) { return impl_->limit_run_to_cycle(deadline); }
