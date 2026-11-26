#ifndef _PCSX_PROF_H_
#define _PCSX_PROF_H_

#include <stdint.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pcsx_prof_state {
	int enabled;
	uint32_t frames;
	uint64_t frame_start_us;
	uint64_t gpu_cmd_us;
	uint64_t vout_us;
	uint64_t spu_us;
	uint64_t rvb_us;
	uint64_t cd_us;
	uint64_t mdec_us;
	uint32_t res_w;
	uint32_t res_h;
	uint32_t prim_f;
	uint32_t prim_ft;
	uint32_t prim_g;
	uint32_t prim_gt;
	uint32_t prim_spr;
	uint32_t prim_tile;
	uint32_t prim_blend;
	uint32_t prim_tex4;
	uint32_t prim_tex8;
	uint32_t prim_tex16;
	uint32_t prim_fb_spans;
	uint32_t prim_gt_promo;
};

extern struct pcsx_prof_state g_pcsx_prof;

static inline uint64_t pcsx_prof_now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void pcsx_prof_frame_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* _PCSX_PROF_H_ */
