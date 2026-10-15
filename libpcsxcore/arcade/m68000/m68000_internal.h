/* MAME-generated MC68000 execution core, isolated from MAME device services. */
#ifndef PCSX_ARCADE_M68000_INTERNAL_H
#define PCSX_ARCADE_M68000_INTERNAL_H
#include "m68000_compat.h"
#include <bit>
#include <cstdint>
#include <functional>
#include <vector>

class m68000_device {
public:
	// Processor special states
	// Must match the states array in m68000gen.py
	enum {
		S_RESET = 0,
		S_BUS_ERROR,
		S_ADDRESS_ERROR,
		S_DOUBLE_FAULT,
		S_INTERRUPT,
		S_TRACE,
		S_ILLEGAL,
		S_PRIVILEDGE,
		S_LINEA,
		S_LINEF,
		S_first_instruction = S_ILLEGAL
	};

	// SR flags
	enum {
		SR_C = 0x0001,
		SR_V = 0x0002,
		SR_Z = 0x0004,
		SR_N = 0x0008,
		SR_X = 0x0010,
		SR_I = 0x0700,
		SR_S = 0x2000,
		SR_T = 0x8000,

		SR_CCR = SR_C|SR_V|SR_Z|SR_N|SR_X,
		SR_SR  = SR_I|SR_S|SR_T,
	};

	// SSW contents
	enum {
		SSW_DATA     = 0x01,
		SSW_PROGRAM  = 0x02,
		SSW_CPU      = 0x03,
		SSW_S        = 0x04,
		SSW_N        = 0x08,
		SSW_R        = 0x10,
		SSW_CRITICAL = 0x20 // Not really part of SSW, go to double fault if that access fails
	};

	// Post-run actions
	enum {
		PR_NONE = 0,
		PR_BERR = 1
	};
    struct decode_entry { u16 value, mask, state; };
    static const decode_entry s_packed_decode_table[];
    using handler = void (m68000_device::*)();
#include <m68000-head.h>
    static const handler s_handlers_df[];
    static const handler s_handlers_dp[];
    const handler *m_handlers_f = nullptr;
    const handler *m_handlers_p = nullptr;

    M68kDelegate<u32, u32> m_cmpild_instr_callback;
    M68kDelegate<int> m_rte_instr_callback;
    M68kDelegate<u32, u16> m_tas_write_callback;
    M68kDelegate<int> m_reset_cb;
    M68kBusAccess m_program, m_opcodes, m_cpu_space;
    M68000Bus m_bus;
    std::vector<u16> m_decode_table;
    u32 m_da[17]{};
    u32 m_ipc=0, m_pc=0, m_au=0, m_at=0, m_aob=0, m_dt=0, m_int_vector=0;
    u32 m_sp=0;
    int m_icount=0, m_bcount=0, m_count_before_instruction_step=0, m_t=0;
    u32 m_movems=0;
    u16 m_isr=0, m_sr=0, m_new_sr=0, m_dbin=0, m_dbout=0, m_edb=0;
    u16 m_irc=0, m_ir=0, m_ird=0, m_ftu=0, m_aluo=0, m_alue=0, m_alub=0, m_movemr=0, m_irdi=0;
    u16 m_base_ssw=0, m_ssw=0;
    u8 m_dcr=0;
    u32 m_virq_state=0, m_nmi_pending=0, m_int_level=0, m_int_next_state=0;
    bool m_nmi_uses_generic=false;
    bool m_disable_interrupt_callback=false;
    u64 m_last_vpa_time=0;
    u16 m_inst_state=0, m_inst_substate=0;
    u32 m_next_state=0, m_post_run=0;
    int m_post_run_cycles=0;

    explicit m68000_device(const M68000Bus &bus);
    void reset();
    void execute_run(int cycles);
    void execute_set_input(int level, int state);
    bool supervisor_mode() const noexcept { return (m_sr & SR_S) != 0; }
    u16 get_fc() const noexcept;
    void set_irq_ack(std::function<void(int)> callback) { m_irq_ack = std::move(callback); }
    u32 pc() const { return m_pc; }
    u16 sr() const { return m_sr; }
    u32 reg(unsigned index) const { return index < 17 ? m_da[index] : 0; }
    u32 active_sp() const { return m_da[m_sp]; }
    bool stopped() const { return m_inst_state == m_decode_table[0x4e72] && !m_int_next_state; }
    bool bus_error() const { return m_bus_error; }
    u64 total_cycles() const { return m_total_cycles; }
    bool current_cycle(u64 &cycle) const;
    bool limit_run_to_cycle(u64 deadline);
    bool callback_active() const { return m_callback_depth != 0; }
    bool callback_fault() const { return m_callback_fault; }
    void note_callback_fault() { m_callback_fault = true; }

    u16 bus_read(M68kSpace space, u32 address, u16 mem_mask);
    void bus_write(M68kSpace space, u32 address, u16 data, u16 mem_mask);
    void debugger_exception_hook(int) {}
    void debugger_wait_hook() {}
    bool access_to_be_redone() const { return false; }

private:
    std::function<void(int)> m_irq_ack;
    bool m_bus_error=false;
    unsigned m_callback_depth=0;
    bool m_callback_fault=false;
    u64 m_total_cycles=0;
    int m_slice_cycles=0;
    bool m_running=false;
    void init_decode_table();
    void abort_access(u32 reason);
    void do_post_run();
    void start_interrupt_vector_lookup();
    void end_interrupt_vector_lookup();
    void update_user_super();
    void update_interrupt();
	// movem step
	inline void step_movem() {
		int r = std::countr_zero(m_movemr);
		if(r > 15)
			r = 0;
		m_movems = map_sp(r);
		m_movemr &= ~(1 << r);
	}

	void step_movem_predec() {
		int r = std::countr_zero(m_movemr);
		if(r > 15)
			r = 0;
		m_movems = map_sp(r ^ 0xf);
		m_movemr &= ~(1 << r);
	}

	// helpers
	static u32 merge_16_32(u16 h, u16 l) { return (h << 16) | l; }
	static u16 high16(u32 v) { return v >> 16; }
	static u32 ext32(u16 v) { return s32(s16(v)); }
	static u16 ext32h(u16 v) { return v & 0x8000 ? 0xffff : 0x0000; }
	static void set_16h(u32 &r, u16 v) { r = (r & 0x0000ffff) | (v << 16); }
	static void set_16l(u32 &r, u16 v) { r = (r & 0xffff0000) | v; }
	static void set_16l(u16 &r, u16 v) { r = v; }
	static void set_8(u32 &r, u8 v) { r = (r & 0xffffff00) | v; }
	static void set_8(u16 &r, u8 v) { r = (r & 0xff00) | v; }
	static void set_8h(u16 &r, u8 v) { r = (r & 0x00ff) | (v << 8); }
	static void set_8xl(u16 &r, u16 v) { r = (v & 0x00ff) | (v << 8); }
	static void set_8xh(u16 &r, u16 v) { r = (v & 0xff00) | (v >> 8); }

	inline int map_sp(int r) { return r == 15 ? m_sp : r; }
	void set_ftu_const();

	inline void alu_add(u16 a, u16 b) {
		u32 r = b + a;
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&a&~r) | ((~b)&(~a)&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_add8(u8 a, u8 b) {
		u16 r = b + a;
		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x100)
			m_isr |= SR_X|SR_C;
		if(((b&a&~r) | ((~b)&(~a)&r)) & 0x80)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_addc(u16 a, u16 b) {
		u32 r = b + a + ((m_isr & SR_C) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&a&~r) | ((~b)&(~a)&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_addx(u16 a, u16 b) {
		u32 r = b + a + ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&a&~r) | ((~b)&(~a)&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_addx8(u8 a, u8 b) {
		u16 r = b + a + ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x100)
			m_isr |= SR_X|SR_C;
		if(((b&a&~r) | ((~b)&(~a)&r)) & 0x80)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_and(u16 a, u16 b) {
		u16 r = b & a;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_andx(u16 a, u16 b) {
		u16 r = b & a;
		m_isr = m_sr & SR_X ? SR_X|SR_C : 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_and8(u16 a, u16 b) {
		u16 r = b & a;
		m_isr = m_sr & SR_X;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_and8x(u8 a, u8 b) {
		u8 r = b & a;
		m_isr = m_sr & SR_X ? SR_X|SR_C : 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_or(u16 a, u16 b) {
		u16 r = b | a;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_or8(u8 a, u8 b) {
		u8 r = b | a;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_eor(u16 a, u16 b) {
		u16 r = b ^ a;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_eor8(u8 a, u8 b) {
		u8 r = b ^ a;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_ext(u16 a) {
		u16 r = s8(a);
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_not(u16 a) {
		u16 r = ~a;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_not8(u8 a) {
		u8 r = ~a;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		m_aluo = r;
	}

	inline void alu_sub(u16 a, u16 b) {
		u32 r = b - a;
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&(~a)&~r) | ((~b)&a&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_sub8(u8 a, u8 b) {
		u16 r = b - a;
		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x100)
			m_isr |= SR_X|SR_C;
		if(((b&(~a)&~r) | ((~b)&a&r)) & 0x80)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_subc(u16 a, u16 b) {
		u32 r = b - a - ((m_isr & SR_C) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&(~a)&~r) | ((~b)&a&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_subx(u16 a, u16 b) {
		u32 r = b - a - ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xffff))
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(r & 0x10000)
			m_isr |= SR_X|SR_C;
		if(((b&(~a)&~r) | ((~b)&a&r)) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_subx8(u8 a, u8 b) {
		u16 r = b - a - ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x100)
			m_isr |= SR_X|SR_C;
		if(((b&(~a)&~r) | ((~b)&a&r)) & 0x80)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_abcd8(u8 a, u8 b) {
		u8 hr = (b & 0xf) + (a & 0xf) + ((m_sr & SR_X) ? 1 : 0);
		bool lcor = hr > 9;
		u16 r1 = b + a + ((m_sr & SR_X) ? 1 : 0);
		u16 r = r1 + (lcor ? 6 : 0);
		if(r > 0x9f)
			r += 0x60;

		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x300)
			m_isr |= SR_X|SR_C;
		if((r & 0x80) && !(r1 & 0x80))
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_sbcd8(u8 a, u8 b) {
		u8 hr = (b & 0xf) - (a & 0xf) - ((m_sr & SR_X) ? 1 : 0);
		bool lcor = hr & 0x10;
		u16 r1 = b - a - ((m_sr & SR_X) ? 1 : 0);
		u16 r = r1 - (lcor ? 6 : 0);
		if(r1 & 0x100)
			r -= 0x60;

		m_isr = 0;
		if(!(r & 0xff))
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(r & 0x300)
			m_isr |= SR_X|SR_C;
		if(!(r & 0x80) && (r1 & 0x80))
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_sla0(u16 a) {
		u32 r = (a << 17) | (m_alue << 1) | 0;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(a & 0x8000)
			m_isr |= SR_C;
		m_alue = r;
		m_aluo = r >> 16;
	}

	inline void alu_sla1(u16 a) {
		u32 r = (a << 17) | (m_alue << 1) | 1;
		m_isr = m_sr & SR_X;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(a & 0x8000)
			m_isr |= SR_C;
		m_alue = r;
		m_aluo = r >> 16;
	}

	// ext with fixed flags result
	inline void alu_over(u16 a) {
		m_isr = SR_V|SR_N;
		m_aluo = s8(a);
	}

	inline void alu_asl(u16 a) {
		u16 r = a << 1;
		m_isr = m_sr & SR_V;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(a & 0x8000)
			m_isr |= SR_X|SR_C;
		if((r^a) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_asl8(u8 a) {
		u8 r = a << 1;
		m_isr = m_sr & SR_V;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(a & 0x80)
			m_isr |= SR_X|SR_C;
		if((r^a) & 0x80)
			m_isr |= SR_V;
		m_aluo = r;
	}

	inline void alu_asl32(u16 a) {
		u32 r = (m_alue << 17) | (a << 1);
		m_isr = m_sr & SR_V;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(m_alue & 0x8000)
			m_isr |= SR_X|SR_C;
		if(((r >> 16)^m_alue) & 0x8000)
			m_isr |= SR_V;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_asr(u16 a) {
		u16 r = a >> 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(a & 0x8000) {
			r |= 0x8000;
			m_isr |= SR_N;
		}
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_asr8(u8 a) {
		u8 r = a >> 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(a & 0x80) {
			r |= 0x80;
			m_isr |= SR_N;
		}
		if(a & 0x01)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_asr32(u16 a) {
		u32 r = (m_alue << 15) | (a >> 1);
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(m_alue & 0x8000) {
			r |= 0x80000000;
			m_isr |= SR_N;
		}
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_lsl(u16 a) {
		u16 r = a << 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(a & 0x8000)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_lsl8(u8 a) {
		u8 r = a << 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80)
			m_isr |= SR_N;
		if(a & 0x80)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_lsl32(u16 a) {
		u32 r = (m_alue << 17) | (a << 1);
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(m_alue & 0x8000)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_lsr(u16 a) {
		u16 r = a >> 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_lsr8(u8 a) {
		u8 r = a >> 1;
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(a & 0x01)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
	}

	inline void alu_lsr32(u16 a) {
		u32 r = (m_alue << 15) | (a >> 1);
		m_isr = 0;
		if(!r)
			m_isr |= SR_Z;
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_rol(u16 a) {
		u16 r = a << 1;
		m_isr = 0;
		if(a & 0x8000) {
			m_isr |= SR_X|SR_C;
			r |= 0x0001;
		}
		if(r & 0x8000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_rol8(u8 a) {
		u8 r = a << 1;
		m_isr = 0;
		if(a & 0x80) {
			m_isr |= SR_X|SR_C;
			r |= 0x01;
		}
		if(r & 0x80)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_rol32(u16 a) {
		u32 r = (m_alue << 17) | (a << 1);
		m_isr = 0;
		if(m_alue & 0x8000) {
			m_isr |= SR_X|SR_C;
			r |= 0x00000001;
		}
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_ror(u16 a) {
		u16 r = a >> 1;
		m_isr = 0;
		if(a & 0x0001) {
			m_isr |= SR_X|SR_C|SR_N;
			r |= 0x8000;
		}
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_ror8(u8 a) {
		u8 r = a >> 1;
		m_isr = 0;
		if(a & 0x01) {
			m_isr |= SR_X|SR_C|SR_N;
			r |= 0x80;
		}
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_ror32(u16 a) {
		u32 r = (m_alue << 15) | (a >> 1);
		m_isr = 0;
		if(a & 0x0001) {
			m_isr |= SR_X|SR_C|SR_N;
			r |= 0x80000000;
		}
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_roxl(u16 a) {
		u16 r = (a << 1) | ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(a & 0x8000)
			m_isr |= SR_X|SR_C;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_roxl8(u8 a) {
		u8 r = (a << 1) | ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(a & 0x80)
			m_isr |= SR_X|SR_C;
		if(r & 0x80)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_roxl32(u16 a) {
		u32 r = (m_alue << 17) | (a << 1) | ((m_sr & SR_X) ? 1 : 0);
		m_isr = 0;
		if(m_alue & 0x8000)
			m_isr |= SR_X|SR_C;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_roxr(u16 a) {
		u16 r = (a >> 1) | ((m_sr & SR_X) ? 0x8000 : 0);
		m_isr = 0;
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		if(r & 0x8000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_roxr8(u8 a) {
		u8 r = (a >> 1) | ((m_sr & SR_X) ? 0x80 : 0);
		m_isr = 0;
		if(a & 0x01)
			m_isr |= SR_X|SR_C;
		if(r & 0x80)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
	}

	inline void alu_roxr32(u32 a) {
		u32 r = (m_alue << 15) | (a >> 1) | ((m_sr & SR_X) ? 0x80000000 : 0);
		m_isr = 0;
		if(a & 0x0001)
			m_isr |= SR_X|SR_C;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(!r)
			m_isr |= SR_Z;
		m_aluo = r;
		m_alue = r >> 16;
	}

	inline void alu_roxr32ms(u16 a) {
		u32 r = (a << 15) | (m_alue >> 1) | (((m_isr & (SR_N|SR_V)) == SR_N || (m_isr & (SR_N|SR_V)) == SR_V) ? 0x80000000 : 0);
		m_isr = 0;
		if(a & 0x0001)
			m_isr |= SR_X;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(!(r & 0xffff0000))
			m_isr |= SR_Z;
		m_aluo = r >> 16;
		m_alue = r;
	}

	inline void alu_roxr32mu(u16 a) {
		u32 r = (a << 15) | (m_alue >> 1) | ((m_isr & SR_C) ? 0x80000000 : 0);
		m_isr = 0;
		if(a & 0x0001)
			m_isr |= SR_X;
		if(r & 0x80000000)
			m_isr |= SR_N;
		if(!(r & 0xffff0000))
			m_isr |= SR_Z;
		m_aluo = r >> 16;
		m_alue = r;
	}

	inline void sr_z() {
		m_sr = (m_sr & ~SR_Z) | (m_isr & SR_Z);
	}

	inline void sr_nz_u() {
		m_sr = (m_sr & ~SR_N & (m_isr | ~SR_Z)) | (m_isr & SR_N);
	}

	inline void sr_nzvc() {
		m_sr = (m_sr & ~(SR_N|SR_Z|SR_V|SR_C)) | (m_isr & (SR_N|SR_Z|SR_V|SR_C));
	}

	inline void sr_nzvc_u() {
		m_sr = (m_sr & ~(SR_N|SR_V|SR_C) & (m_isr | ~SR_Z)) | (m_isr & (SR_N|SR_V|SR_C));
	}

	inline void sr_xnzvc() {
		m_sr = (m_sr & ~(SR_X|SR_N|SR_Z|SR_V|SR_C)) | (m_isr & (SR_X|SR_N|SR_Z|SR_V|SR_C));
	}

	inline void sr_xnzvc_u() {
		m_sr = (m_sr & ~(SR_X|SR_N|SR_V|SR_C) & (m_isr | ~SR_Z)) | (m_isr & (SR_X|SR_N|SR_V|SR_C));
	}
};
#endif
