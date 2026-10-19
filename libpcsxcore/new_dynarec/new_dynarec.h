#define NEW_DYNAREC 1

#define MAXBLOCK 2048 // in mips instructions

#define NDHACK_NO_SMC_CHECK     (1u << 0)
#define NDHACK_GTE_UNNEEDED     (1u << 1)
#define NDHACK_GTE_NO_FLAGS     (1u << 2)
#define NDHACK_OVERRIDE_CYCLE_M (1u << 3)
#define NDHACK_NO_STALLS        (1u << 4)
#define NDHACK_NO_COMPAT_HACKS  (1u << 5)
#define NDHACK_THREAD_FORCE     (1u << 6)
#define NDHACK_THREAD_FORCE_ON  (1u << 7)
#define NDHACK_NO_LD_DELAY      (1u << 8)

struct ndrc_globals
{
	int hacks;
	int hacks_pergame;
	int hacks_old;
	int did_compile;
	int cycle_multiplier_old;
	struct {
		void *handle;
		void *lock;
		void *cond;
		void *dirty_start;
		void *dirty_end;
		unsigned int busy_addr; // 0 is valid, ~0 == none
		int exit;
	} thread;
};
extern struct ndrc_globals ndrc_g;

void new_dynarec_init(void);
void new_dynarec_cleanup(void);
void new_dynarec_clear_full(void);
int  new_dynarec_save_blocks(void *save, int size);
void new_dynarec_load_blocks(const void *save, int size);
void new_dynarec_print_stats(void);
int  new_dynarec_estimate_stack_size(void);

int  new_dynarec_quick_check_range(unsigned int start, unsigned int end);
void new_dynarec_invalidate_range(unsigned int start, unsigned int end);
/* Arcade boards may change the backing bytes for one guest address (ROM bank
 * switches). This invalidates translations without allowing old-source block
 * restoration. The source callback returns a stable word span [pc, limit),
 * zero outside its window, or -1 when opcode fetch has bus side effects and
 * must execute live through the interpreter instead of speculative decoding. */
typedef int (*ndrc_code_span_fn)(unsigned int pc, const unsigned int **words,
	unsigned int *limit);
void new_dynarec_set_code_span_provider(ndrc_code_span_fn provider);
int  new_dynarec_arcade_memory_enabled(void);
int  new_dynarec_arcade_selftest(void);
void new_dynarec_invalidate_range_hard(unsigned int start, unsigned int end);
void new_dynarec_invalidate_range_hard_internal(unsigned int start, unsigned int end);
int  new_dyna_pcsx_mem_arcade_selftest(void);
void new_dynarec_invalidate_all_pages(void);
void new_dyna_clear_cache(void *start, void *end);

void new_dyna_start(void *context);
void new_dyna_start_at(void *context, void *compiled_code);

struct ht_entry;
enum ndrc_compile_mode {
	ndrc_cm_no_compile = 0,
	ndrc_cm_compile_live,       // from executing code, vaddr is the current pc
	ndrc_cm_compile_offline,
	ndrc_cm_compile_in_thread,
};
void *ndrc_get_addr_ht_param(struct ht_entry *ht, unsigned int vaddr,
	enum ndrc_compile_mode compile_mode);

extern unsigned int ndrc_smrv_regs[32];
