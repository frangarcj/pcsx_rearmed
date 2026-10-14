#ifndef __P_OUT_H__
#define __P_OUT_H__

struct out_driver {
	const char *name;
	int (*init)(void);
	void (*finish)(void);
	int (*busy)(void);
	void (*feed)(void *data, int bytes);
};

extern struct out_driver *out_current;

void SetupSound(void);
/* Read-only PCM end timestamp during feed(), in the SPU/core cycle domain. */
unsigned int dfsound_output_end_cycle(void);

#endif /* __P_OUT_H__ */
