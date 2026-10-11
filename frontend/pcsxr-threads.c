#ifndef _GNU_SOURCE
#define _GNU_SOURCE // *_np
#endif
#ifdef _3DS
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/services/apt.h>
#include <sys/time.h>
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
static bool is_new_3ds;
#endif
#include "pcsxr-threads.h"

int pcsxr_sthread_core_count;
int pcsxr_gpu2_core = -1;
/* gpulib's GPU thread's core with the vitaGL renderer (-1: core 2, with
 * the board); the frontend may choose another for measurements. */
int pcsxr_gpu_core = -1;
/* The same for the board thread (-1: core 2). */
int pcsxr_board_core = -1;

// pcsxr "extensions"
extern void SysPrintf(const char *fmt, ...);

#ifndef USE_C11_THREADS

#include "../deps/libretro-common/rthreads/rthreads.c"
#include "features/features_cpu.h"

#define CORE_COUNT() cpu_features_get_core_amount()

static void pcsxr_sthread_lib_init(void)
{
#ifdef _3DS
	int64_t version = 0;
	int fpscr = -1;

	APT_CheckNew3DS(&is_new_3ds);
	svcGetSystemInfo(&version, 0x10000, 0);

	APT_SetAppCpuTimeLimit(35);
	u32 percent = -1;
	APT_GetAppCpuTimeLimit(&percent);

	__asm__ volatile("fmrx %0, fpscr" : "=r"(fpscr));
	SysPrintf("%s3ds detected, v%d.%d, AppCpuTimeLimit=%ld fpscr=%08x\n",
		is_new_3ds ? "new" : "old", (int)GET_VERSION_MAJOR(version),
		(int)GET_VERSION_MINOR(version), percent, fpscr);
#endif
}

sthread_t *pcsxr_sthread_create(void (*thread_func)(void *),
	enum pcsxr_thread_type type)
{
	sthread_t *h = NULL;
#ifdef _3DS
	size_t stack_size = 64*1024;
	Thread ctr_thread;
	int core_id = 0;
	s32 prio = 0x30;

	h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

	switch (type) {
	case PCSXRT_CDR:
	case PCSXRT_SPU:
	case PCSXRT_BOARD:
		core_id = 1;
		break;
	case PCSXRT_DRC:
		stack_size = new_dynarec_estimate_stack_size();
		// fallthrough
	case PCSXRT_GPU:
	case PCSXRT_GPU2:
		core_id = is_new_3ds ? 2 : 1;
		break;
	case PCSXRT_COUNT:
		break;
	}

	ctr_thread = threadCreate(thread_func, NULL, stack_size, prio, core_id, false);
	if (!ctr_thread) {
		if (core_id == 1) {
			SysPrintf("threadCreate pcsxt %d core %d failed\n",
				type, core_id);
			core_id = is_new_3ds ? 2 : -1;
			ctr_thread = threadCreate(thread_func, NULL, stack_size,
				prio, core_id, false);
		}
	}
	SysPrintf("threadCreate: pcsxt %d core %d stack %zd: %p\n",
		type, core_id, stack_size, ctr_thread);
	if (!ctr_thread) {
		free(h);
		return NULL;
	}
	h->id = ctr_thread;
#else
	h = sthread_create(thread_func, NULL);
 #ifdef VITA
	/* Main emulation stays on core 0 (set by the frontend). With the vitaGL
	 * renderer its own thread has core 1, so the GPU thread (building frame
	 * packets) shares core 2 with the board. */
#ifdef GPU_VITA_CTR
	if (h && (type == PCSXRT_GPU || type == PCSXRT_BOARD))
		sceKernelChangeThreadCpuAffinityMask(h->id,
			type == PCSXRT_GPU && pcsxr_gpu_core >= 0 && pcsxr_gpu_core <= 3
			? SCE_KERNEL_CPU_MASK_USER_0 << pcsxr_gpu_core
			: type == PCSXRT_BOARD && pcsxr_board_core >= 0 && pcsxr_board_core <= 3
			? SCE_KERNEL_CPU_MASK_USER_0 << pcsxr_board_core : SCE_KERNEL_CPU_MASK_USER_2);
	/* The board thread falls behind the emulation as soon as it waits for
	 * the core; the GPU thread has slack (lower number, higher priority). */
	if (h && type == PCSXRT_GPU)
	{
		const int prio = sceKernelGetThreadCurrentPriority() + 1;
		sceKernelChangeThreadPriority(h->id, prio > 191 ? 191 : prio);
	}
#else
	if (h && (type == PCSXRT_GPU || type == PCSXRT_BOARD))
		sceKernelChangeThreadCpuAffinityMask(h->id, type == PCSXRT_GPU
			? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_2);
#endif
	/* gpu_neon's second render stage: the frontend picks the core. */
	if (h && type == PCSXRT_GPU2 && pcsxr_gpu2_core >= 0 && pcsxr_gpu2_core <= 2)
		sceKernelChangeThreadCpuAffinityMask(h->id,
			SCE_KERNEL_CPU_MASK_USER_0 << pcsxr_gpu2_core);
 #endif
 #if defined(__GLIBC__) || \
    (defined(__ANDROID_API__) && __ANDROID_API__ >= 26)
	if (h && (unsigned int)type < (unsigned int)PCSXRT_COUNT)
	{
		const char * const pcsxr_tnames[PCSXRT_COUNT] = {
			"pcsxr-cdrom", "pcsxr-drc", "pcsxr-gpu", "pcsxr-spu",
			"pcsxr-board", "pcsxr-gpu2"
		};
		pthread_setname_np(h->id, pcsxr_tnames[type]);
	}
 #endif
#endif
	return h;
}

#else // USE_C11_THREADS

#include <unistd.h>
#define CORE_COUNT() sysconf(_SC_NPROCESSORS_ONLN)
#define pcsxr_sthread_lib_init()

#endif

void pcsxr_sthread_init(void)
{
	pcsxr_sthread_core_count = CORE_COUNT();
	if (pcsxr_sthread_core_count < 1)
		pcsxr_sthread_core_count = 1;
	SysPrintf("%d cpu core(s) detected\n", pcsxr_sthread_core_count);

	pcsxr_sthread_lib_init();
}
