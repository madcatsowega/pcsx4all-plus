/*
 * Unified JZ4760B 330 MHz AUX+MXU Coprocessor Manager for PCSX4All+
 *
 * Manages /dev/jz_aux with a single 2-slot zero-wait-state FIFO ring
 * (Slot 0 = 16 KB TCSM0, Slot 1 = upper 16 KB TCSM1, 12 KB SRAM Bank 0)
 * shared concurrently by PS1 SPU (spu_pcsxrearmed) and PS1 MDEC (mdec.cpp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "pcsx_aux.h"
#include "pcsx_prof.h"

struct pcsx_prof_state g_pcsx_prof;

void pcsx_prof_frame_tick(void)
{
	static int checked = 0;
	static uint32_t bench_target = 0;
	static uint32_t bench_warmup = 30;
	static uint32_t bench_seen = 0;
	static uint64_t bench_t0 = 0;
	static uint64_t b_gpu_us = 0, b_vout_us = 0, b_spu_us = 0, b_rvb_us = 0, b_cd_us = 0, b_mdec_us = 0;
	if (!checked) {
		checked = 1;
		const char *bf = getenv("PCSX_BENCH_FRAMES");
		if (bf && atoi(bf) > 0)
			bench_target = (uint32_t)atoi(bf);
		g_pcsx_prof.enabled = (bench_target > 0 ||
		                       getenv("PCSX_PROF") != NULL ||
		                       access("/tmp/pcsx_prof.enable", F_OK) == 0);
		if (g_pcsx_prof.enabled) {
			g_pcsx_prof.frame_start_us = pcsx_prof_now_us();
			FILE *f = fopen("/tmp/pcsx_prof.log", "w");
			if (f) { fprintf(f, "# PCSX4All+ Live Frame Profiler\n"); fclose(f); }
		}
	}
	if (!g_pcsx_prof.enabled)
		return;

	if (bench_target > 0) {
		bench_seen++;
		if (bench_seen == bench_warmup) {
			bench_t0 = pcsx_prof_now_us();
			b_gpu_us = b_vout_us = b_spu_us = b_rvb_us = b_cd_us = b_mdec_us = 0;
			g_pcsx_prof.gpu_cmd_us = g_pcsx_prof.vout_us = g_pcsx_prof.spu_us = 0;
			g_pcsx_prof.rvb_us = g_pcsx_prof.cd_us = g_pcsx_prof.mdec_us = 0;
		} else if (bench_seen > bench_warmup) {
			b_gpu_us  += g_pcsx_prof.gpu_cmd_us;  g_pcsx_prof.gpu_cmd_us = 0;
			b_vout_us += g_pcsx_prof.vout_us;     g_pcsx_prof.vout_us = 0;
			b_spu_us  += g_pcsx_prof.spu_us;      g_pcsx_prof.spu_us = 0;
			b_rvb_us  += g_pcsx_prof.rvb_us;      g_pcsx_prof.rvb_us = 0;
			b_cd_us   += g_pcsx_prof.cd_us;       g_pcsx_prof.cd_us = 0;
			b_mdec_us += g_pcsx_prof.mdec_us;     g_pcsx_prof.mdec_us = 0;
			if (bench_seen >= bench_warmup + bench_target) {
				uint64_t span = pcsx_prof_now_us() - bench_t0;
				if (span == 0) span = 1;
				double f = (double)bench_target;
				double fps = (f * 1000000.0) / (double)span;
				double fms = (double)span / (f * 1000.0);
				double gms = (double)b_gpu_us / (f * 1000.0);
				double vms = (double)b_vout_us / (f * 1000.0);
				double sms = (double)b_spu_us / (f * 1000.0);
				double rms = (double)b_rvb_us / (f * 1000.0);
				double cms = (double)b_cd_us / (f * 1000.0);
				double mms = (double)b_mdec_us / (f * 1000.0);
				double cpu = fms - gms - vms - sms - cms - mms;
				if (cpu < 0.0) cpu = 0.0;
				uint32_t aspu = 0, amdec = 0, awaits = 0;
				pcsx_aux_get_stats(&aspu, &amdec, &awaits);
				printf("[BENCH] %ux%u frames=%u elapsed=%.1fms FPS=%.2f frame=%.2fms | gpu=%.2f vout=%.2f spu=%.2f(rvb=%.2f) cd=%.2f mdec=%.2f cpu+gte=%.2f | aux: spu=%u mdec=%u w=%u\n",
				       g_pcsx_prof.res_w, g_pcsx_prof.res_h, bench_target,
				       (double)span / 1000.0, fps, fms, gms, vms, sms, rms, cms, mms, cpu,
				       aspu, amdec, awaits);
				fflush(stdout);
				exit(0);
			}
		}
		return;
	}

	g_pcsx_prof.frames++;
	if (g_pcsx_prof.frames >= 60) {
		uint64_t now = pcsx_prof_now_us();
		uint64_t span = now - g_pcsx_prof.frame_start_us;
		if (span == 0) span = 1;
		double fps = (60.0 * 1000000.0) / (double)span;
		double frame_ms = (double)span / (60.0 * 1000.0);
		double gpu_ms   = (double)g_pcsx_prof.gpu_cmd_us / (60.0 * 1000.0);
		double vout_ms  = (double)g_pcsx_prof.vout_us / (60.0 * 1000.0);
		double spu_ms   = (double)g_pcsx_prof.spu_us / (60.0 * 1000.0);
		double rvb_ms   = (double)g_pcsx_prof.rvb_us / (60.0 * 1000.0);
		double cd_ms    = (double)g_pcsx_prof.cd_us / (60.0 * 1000.0);
		double mdec_ms  = (double)g_pcsx_prof.mdec_us / (60.0 * 1000.0);
		double cpu_ms   = frame_ms - gpu_ms - vout_ms - spu_ms - cd_ms - mdec_ms;
		if (cpu_ms < 0.0) cpu_ms = 0.0;

		uint32_t aspu = 0, amdec = 0, awaits = 0;
		pcsx_aux_get_stats(&aspu, &amdec, &awaits);

		FILE *f = fopen("/tmp/pcsx_prof.log", "a");
		if (f) {
			fprintf(f, "FPS=%5.1f %ux%u frame=%5.2fms | gpu=%5.2f vout=%4.2f spu=%4.2f(rvb=%4.2f) cd=%4.2f mdec=%4.2f cpu+gte=%5.2f | "
			           "prims/f: F=%u FT=%u G=%u GT=%u(p=%u) Spr=%u Tile=%u Blend=%u (4b=%u 8b=%u 16b=%u fb=%u) | "
			           "aux: spu=%u mdec=%u w=%u\n",
			        fps, g_pcsx_prof.res_w, g_pcsx_prof.res_h,
			        frame_ms, gpu_ms, vout_ms, spu_ms, rvb_ms, cd_ms, mdec_ms, cpu_ms,
			        g_pcsx_prof.prim_f / 60, g_pcsx_prof.prim_ft / 60,
			        g_pcsx_prof.prim_g / 60, g_pcsx_prof.prim_gt / 60,
			        g_pcsx_prof.prim_gt_promo / 60,
			        g_pcsx_prof.prim_spr / 60, g_pcsx_prof.prim_tile / 60,
			        g_pcsx_prof.prim_blend / 60,
			        g_pcsx_prof.prim_tex4 / 60, g_pcsx_prof.prim_tex8 / 60, g_pcsx_prof.prim_tex16 / 60,
			        g_pcsx_prof.prim_fb_spans / 60,
			        aspu, amdec, awaits);
			fclose(f);
		}

		g_pcsx_prof.frames = 0;
		g_pcsx_prof.frame_start_us = now;
		g_pcsx_prof.gpu_cmd_us = 0;
		g_pcsx_prof.vout_us = 0;
		g_pcsx_prof.spu_us = 0;
		g_pcsx_prof.rvb_us = 0;
		g_pcsx_prof.cd_us = 0;
		g_pcsx_prof.mdec_us = 0;
		g_pcsx_prof.prim_f = g_pcsx_prof.prim_ft = 0;
		g_pcsx_prof.prim_g = g_pcsx_prof.prim_gt = 0;
		g_pcsx_prof.prim_spr = g_pcsx_prof.prim_tile = 0;
		g_pcsx_prof.prim_blend = 0;
		g_pcsx_prof.prim_tex4 = g_pcsx_prof.prim_tex8 = g_pcsx_prof.prim_tex16 = 0;
		g_pcsx_prof.prim_fb_spans = g_pcsx_prof.prim_gt_promo = 0;
	}
}

#ifdef RS97
#include <libjzaux.h>
#include "/home/sean/retrofw-aux/firmware/pcsx_hybrid_proto.h"
#include "/home/sean/retrofw-aux/firmware/aux_pcsx_hybrid_fw.h"

struct mdec_pending_slot {
	uint8_t *dst;
	uint32_t out_bytes;
};

static jz_aux_ctx_t *g_aux_ctx = NULL;
static volatile struct aux_hybrid_sram *g_aux_sram = NULL;
static volatile union aux_hybrid_slot *g_aux_slots[2] = { NULL, NULL };

static uint32_t g_submit_seq = 0;
static uint32_t g_reap_seq = 0;
static uint32_t g_slot_owner[2] = { AUX_CMD_NONE, AUX_CMD_NONE };
static struct mdec_pending_slot g_mdec_pend[2];
static int g_mdec_inflight = 0;
static int g_aux_active = 0;

static pcsx_aux_spu_reap_fn g_spu_reap_cb = NULL;

static uint32_t g_prof_spu_jobs = 0;
static uint32_t g_prof_mdec_jobs = 0;
static uint32_t g_prof_waits = 0;

static void copy_from_tcsm_u8(uint8_t *dst, const volatile uint8_t *src, uint32_t n)
{
	if ((((uintptr_t)dst | (uintptr_t)src) & 3U) == 0) {
		uint32_t *d32 = (uint32_t *)dst;
		const volatile uint32_t *s32 = (const volatile uint32_t *)src;
		uint32_t nw = n >> 2;
		while (nw >= 4) {
			d32[0] = s32[0];
			d32[1] = s32[1];
			d32[2] = s32[2];
			d32[3] = s32[3];
			d32 += 4;
			s32 += 4;
			nw -= 4;
		}
		while (nw--)
			*d32++ = *s32++;
		dst = (uint8_t *)d32;
		src = (const volatile uint8_t *)s32;
		n &= 3U;
	}
	while (n--)
		*dst++ = *src++;
}

static void copy_to_tcsm_u16(volatile uint16_t *dst, const uint16_t *src, uint32_t nwords)
{
	if ((((uintptr_t)dst | (uintptr_t)src) & 3U) == 0) {
		volatile uint32_t *d32 = (volatile uint32_t *)dst;
		const uint32_t *s32 = (const uint32_t *)src;
		uint32_t n32 = nwords >> 1;
		while (n32 >= 4) {
			d32[0] = s32[0];
			d32[1] = s32[1];
			d32[2] = s32[2];
			d32[3] = s32[3];
			d32 += 4;
			s32 += 4;
			n32 -= 4;
		}
		while (n32--)
			*d32++ = *s32++;
		if (nwords & 1U)
			dst[nwords - 1] = src[nwords - 1];
		return;
	}
	for (uint32_t i = 0; i < nwords; i++)
		dst[i] = src[i];
}

static void pcsx_aux_reap_one(void)
{
	uint32_t seq = g_reap_seq;
	uint32_t idx = seq & 1U;
	int timeout = 5000000;
	int waited = 0;

	while (g_aux_sram->done_seq <= seq) {
		waited = 1;
		if (--timeout <= 0) {
			printf("[pcsx-aux] WARNING: Hybrid AUX timeout (done=%u want=%u cmd=%u), disabling AUX!\n",
			       g_aux_sram->done_seq, seq + 1, g_slot_owner[idx]);
			FILE *lf = fopen("/tmp/pcsx_aux.log", "a");
			if (lf) {
				fprintf(lf, "TIMEOUT done=%u want=%u cmd=%u spu=%u mdec=%u\n",
				        g_aux_sram->done_seq, seq + 1, g_slot_owner[idx],
				        g_prof_spu_jobs, g_prof_mdec_jobs);
				fclose(lf);
			}
			g_aux_active = 0;
			g_reap_seq = g_submit_seq;
			g_mdec_inflight = 0;
			return;
		}
	}
	if (waited)
		g_prof_waits++;

	uint32_t owner = g_slot_owner[idx];
	g_slot_owner[idx] = AUX_CMD_NONE;
	g_reap_seq++;

	if (owner == AUX_CMD_SPU) {
		g_prof_spu_jobs++;
		if (g_spu_reap_cb)
			g_spu_reap_cb(idx);
	} else if (owner == AUX_CMD_MDEC) {
		struct mdec_pending_slot *mp = &g_mdec_pend[idx];
		if (mp->dst && mp->out_bytes) {
			copy_from_tcsm_u8(mp->dst, g_aux_slots[idx]->mdec.out_px, mp->out_bytes);
		}
		mp->dst = NULL;
		mp->out_bytes = 0;
		if (g_mdec_inflight > 0)
			g_mdec_inflight--;
		g_prof_mdec_jobs++;
	}
}

void pcsx_aux_reap_ready(void)
{
	if (!g_aux_active)
		return;
	while (g_reap_seq != g_submit_seq && g_aux_sram->done_seq > g_reap_seq)
		pcsx_aux_reap_one();
}

void pcsx_aux_flush(void)
{
	if (!g_aux_active)
		return;
	while (g_reap_seq != g_submit_seq && g_aux_active)
		pcsx_aux_reap_one();
}

void pcsx_aux_flush_mdec(void)
{
	if (!g_aux_active || g_mdec_inflight == 0)
		return;
	while (g_mdec_inflight > 0 && g_reap_seq != g_submit_seq && g_aux_active)
		pcsx_aux_reap_one();
}

int pcsx_aux_is_active(void)
{
	return g_aux_active;
}

void pcsx_aux_register_spu(pcsx_aux_spu_reap_fn reap_fn)
{
	g_spu_reap_cb = reap_fn;
}

void *pcsx_aux_get_sram(void)
{
	return (void *)g_aux_sram;
}

void *pcsx_aux_get_slot(uint32_t slot_idx)
{
	return (void *)g_aux_slots[slot_idx & 1U];
}

int pcsx_aux_acquire_slot(uint32_t *out_slot_idx)
{
	if (!g_aux_active)
		return 0;
	pcsx_aux_reap_ready();
	if ((g_submit_seq - g_reap_seq) >= 2U)
		pcsx_aux_reap_one();
	if (!g_aux_active)
		return 0;
	*out_slot_idx = g_submit_seq & 1U;
	return 1;
}

void pcsx_aux_submit_slot(uint32_t slot_idx, uint32_t cmd_type)
{
	slot_idx &= 1U;
	g_slot_owner[slot_idx] = cmd_type;
	g_aux_sram->slot_cmd[slot_idx] = cmd_type;
	__sync_synchronize();
	g_submit_seq++;
	g_aux_sram->submit_seq = g_submit_seq;
}

void pcsx_aux_mdec_set_iq(const int *iq_y, const int *iq_uv)
{
	if (!g_aux_active || !g_aux_sram)
		return;
	pcsx_aux_flush_mdec();
	if (!g_aux_active)
		return;
	for (int i = 0; i < 64; i++) {
		g_aux_sram->iq_y[i]  = (int32_t)iq_y[i];
		g_aux_sram->iq_uv[i] = (int32_t)iq_uv[i];
	}
	__sync_synchronize();
}

int pcsx_aux_mdec_submit(const uint16_t *rl_src, uint32_t rl_words,
                         uint32_t n_blocks, uint32_t flags,
                         uint8_t *dst_out, uint32_t out_bytes)
{
	if (!g_aux_active || n_blocks == 0 || rl_words == 0 ||
	    rl_words > AUX_MDEC_MAX_RL || out_bytes > AUX_MDEC_MAX_OUT)
		return 0;

	uint32_t idx;
	if (!pcsx_aux_acquire_slot(&idx))
		return 0;

	volatile struct aux_mdec_slot *ms = &g_aux_slots[idx]->mdec;
	ms->n_blocks     = n_blocks;
	ms->flags        = flags;
	ms->rl_max_words = rl_words;
	ms->rl_consumed  = 0;
	ms->blocks_done  = 0;
	copy_to_tcsm_u16(ms->rl_in, rl_src, rl_words);

	g_mdec_pend[idx].dst       = dst_out;
	g_mdec_pend[idx].out_bytes = out_bytes;
	g_mdec_inflight++;

	pcsx_aux_submit_slot(idx, AUX_CMD_MDEC);
	return 1;
}

void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits)
{
	if (spu_jobs)  *spu_jobs  = g_prof_spu_jobs;
	if (mdec_jobs) *mdec_jobs = g_prof_mdec_jobs;
	if (waits)     *waits     = g_prof_waits;
}

void pcsx_aux_init(void)
{
	if (g_aux_ctx)
		return;

	jz_aux_ctx_t *ctx = jz_aux_open();
	if (!ctx) {
		printf("-> /dev/jz_aux not available; SPU & MDEC running on main CPU.\n");
		return;
	}
	if (!ctx->sram || ctx->sram_mapped_size < sizeof(struct aux_hybrid_sram)) {
		printf("-> /dev/jz_aux SRAM too small (%u bytes); running on main CPU.\n",
		       (unsigned)ctx->sram_mapped_size);
		jz_aux_close(ctx);
		return;
	}
	volatile uint8_t *tcsm1 = (volatile uint8_t *)jz_aux_map_tcsm1(ctx);
	if (!tcsm1) {
		printf("-> /dev/jz_aux TCSM1 mmap failed; running on main CPU.\n");
		jz_aux_close(ctx);
		return;
	}

	volatile uint32_t *s32 = (volatile uint32_t *)ctx->sram;
	for (size_t i = 0; i < (sizeof(struct aux_hybrid_sram) + 3) / 4; i++)
		s32[i] = 0;

	if (jz_aux_load(ctx, aux_pcsx_hybrid_fw, sizeof(aux_pcsx_hybrid_fw), 1, 1) < 0) {
		printf("-> /dev/jz_aux hybrid firmware load failed; running on main CPU.\n");
		jz_aux_close(ctx);
		return;
	}

	volatile struct aux_hybrid_sram *sram = (volatile struct aux_hybrid_sram *)ctx->sram;
	int tries = 300000;
	while (sram->magic != AUX_HYBRID_MAGIC && --tries > 0)
		;
	if (sram->magic != AUX_HYBRID_MAGIC) {
		printf("-> /dev/jz_aux hybrid firmware boot timeout (magic=0x%08x).\n", sram->magic);
		jz_aux_close(ctx);
		return;
	}

	g_aux_ctx      = ctx;
	g_aux_sram     = sram;
	g_aux_slots[0] = (volatile union aux_hybrid_slot *)ctx->tcsm0;
	g_aux_slots[1] = (volatile union aux_hybrid_slot *)(tcsm1 + AUX_HYB_SLOT1_OFFSET);
	g_submit_seq   = 0;
	g_reap_seq     = 0;
	g_slot_owner[0] = AUX_CMD_NONE;
	g_slot_owner[1] = AUX_CMD_NONE;
	g_mdec_inflight = 0;
	g_prof_spu_jobs = 0;
	g_prof_mdec_jobs = 0;
	g_prof_waits   = 0;
	g_aux_active   = 1;

	printf("-> JZ4760B HYBRID AUX+MXU COPROCESSOR ACTIVE! (%u MHz HCLK, fw=%uB, SPU+MDEC 2-slot ring)\n",
	       ctx->info.hclk_mhz, (unsigned)sizeof(aux_pcsx_hybrid_fw));
	FILE *lf = fopen("/tmp/pcsx_aux.log", "w");
	if (lf) {
		fprintf(lf, "ACTIVE HYBRID hclk=%uMHz cclk=%uMHz fw=%uB\n",
		        ctx->info.hclk_mhz, ctx->info.cclk_mhz, (unsigned)sizeof(aux_pcsx_hybrid_fw));
		fclose(lf);
	}
}

void pcsx_aux_shutdown(void)
{
	if (!g_aux_ctx)
		return;
	pcsx_aux_flush();
	printf("-> JZ4760B Hybrid AUX stats: spu=%u mdec=%u waits=%u\n",
	       g_prof_spu_jobs, g_prof_mdec_jobs, g_prof_waits);
	FILE *lf = fopen("/tmp/pcsx_aux.log", "a");
	if (lf) {
		fprintf(lf, "SHUTDOWN spu=%u mdec=%u waits=%u\n",
		        g_prof_spu_jobs, g_prof_mdec_jobs, g_prof_waits);
		fclose(lf);
	}
	g_aux_active = 0;
	jz_aux_close(g_aux_ctx);
	g_aux_ctx = NULL;
	g_aux_sram = NULL;
	g_aux_slots[0] = NULL;
	g_aux_slots[1] = NULL;
}

#else /* !RS97 */

void pcsx_aux_init(void) {}
void pcsx_aux_shutdown(void) {}
int  pcsx_aux_is_active(void) { return 0; }
void pcsx_aux_reap_ready(void) {}
void pcsx_aux_flush(void) {}
void pcsx_aux_flush_mdec(void) {}
void pcsx_aux_register_spu(pcsx_aux_spu_reap_fn reap_fn) { (void)reap_fn; }
void *pcsx_aux_get_sram(void) { return NULL; }
void *pcsx_aux_get_slot(uint32_t slot_idx) { (void)slot_idx; return NULL; }
int   pcsx_aux_acquire_slot(uint32_t *out_slot_idx) { (void)out_slot_idx; return 0; }
void  pcsx_aux_submit_slot(uint32_t slot_idx, uint32_t cmd_type) { (void)slot_idx; (void)cmd_type; }
void pcsx_aux_mdec_set_iq(const int *iq_y, const int *iq_uv) { (void)iq_y; (void)iq_uv; }
int  pcsx_aux_mdec_submit(const uint16_t *rl_src, uint32_t rl_words,
                          uint32_t n_blocks, uint32_t flags,
                          uint8_t *dst_out, uint32_t out_bytes)
{
	(void)rl_src; (void)rl_words; (void)n_blocks; (void)flags; (void)dst_out; (void)out_bytes;
	return 0;
}
void pcsx_aux_get_stats(uint32_t *spu_jobs, uint32_t *mdec_jobs, uint32_t *waits)
{
	if (spu_jobs)  *spu_jobs  = 0;
	if (mdec_jobs) *mdec_jobs = 0;
	if (waits)     *waits     = 0;
}
#endif
